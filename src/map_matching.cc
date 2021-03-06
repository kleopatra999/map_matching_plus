#include <valhalla/sif/autocost.h>
#include <valhalla/sif/bicyclecost.h>
#include <valhalla/sif/pedestriancost.h>
#include <valhalla/midgard/logging.h>

using namespace valhalla;

#include "mmp/candidate_search.h"
#include "mmp/universal_cost.h"
#include "mmp/routing.h"
#include "mmp/graph_helpers.h"
#include "mmp/geometry_helpers.h"
#include "mmp/map_matching.h"

using ptree = boost::property_tree::ptree;


namespace mmp {

State::State(const StateId id,
             const Time time,
             const Candidate& candidate)
    : id_(id),
      time_(time),
      candidate_(candidate),
      labelset_(nullptr),
      label_idx_() {}


void
State::route(const std::vector<const State*>& states,
             baldr::GraphReader& graphreader,
             float max_route_distance,
             const midgard::DistanceApproximator& approximator,
             float search_radius,
             sif::cost_ptr_t costing,
             std::shared_ptr<const sif::EdgeLabel> edgelabel,
             const float turn_cost_table[181]) const
{
  // Prepare locations
  std::vector<baldr::PathLocation> locations;
  locations.reserve(1 + states.size());
  locations.push_back(candidate_);
  for (const auto state : states) {
    locations.push_back(state->candidate());
  }

  // Route
  labelset_ = std::make_shared<LabelSet>(std::ceil(max_route_distance));
  // TODO pass labelset_ as shared_ptr
  const auto& results = find_shortest_path(
      graphreader, locations, 0, *labelset_,
      approximator, search_radius,
      costing, edgelabel, turn_cost_table);

  // Cache results
  label_idx_.clear();
  uint16_t dest = 1;  // dest at 0 is remained for the origin
  for (const auto state : states) {
    const auto it = results.find(dest);
    if (it != results.end()) {
      label_idx_[state->id()] = it->second;
    }
    dest++;
  }
}


const Label*
State::last_label(const State& state) const
{
  const auto it = label_idx_.find(state.id());
  if (it != label_idx_.end()) {
    return &labelset_->label(it->second);
  }
  return nullptr;
}


inline float
GreatCircleDistance(const State& left,
                    const State& right)
{
  const auto &left_pt = left.candidate().vertex(),
            &right_pt = right.candidate().vertex();
  return left_pt.Distance(right_pt);
}


inline float
GreatCircleDistanceSquared(const State& left,
                           const State& right)
{
  const auto &left_pt = left.candidate().vertex(),
            &right_pt = right.candidate().vertex();
  return left_pt.DistanceSquared(right_pt);
}


inline float
GreatCircleDistance(const Measurement& left,
                    const Measurement& right)
{ return left.lnglat().Distance(right.lnglat()); }


inline float
GreatCircleDistanceSquared(const Measurement& left,
                           const Measurement& right)
{ return left.lnglat().DistanceSquared(right.lnglat()); }


MapMatching::MapMatching(baldr::GraphReader& graphreader,
                         const sif::cost_ptr_t* mode_costing,
                         const sif::TravelMode mode,
                         float sigma_z,
                         float beta,
                         float breakage_distance,
                         float max_route_distance_factor,
                         float search_radius,
                         float turn_penalty_factor)
    : graphreader_(graphreader),
      mode_costing_(mode_costing),
      mode_(mode),
      measurements_(),
      states_(),
      sigma_z_(sigma_z),
      inv_double_sq_sigma_z_(1.f / (sigma_z_ * sigma_z_ * 2.f)),
      beta_(beta),
      inv_beta_(1.f / beta_),
      breakage_distance_(breakage_distance),
      max_route_distance_factor_(max_route_distance_factor),
      search_radius_(search_radius),
      turn_penalty_factor_(turn_penalty_factor),
      turn_cost_table_{0.f}
{
  if (sigma_z_ <= 0.f) {
    throw std::invalid_argument("Expect sigma_z to be positive");
  }

  if (beta_ <= 0.f) {
    throw std::invalid_argument("Expect beta to be positive");
  }

  if (search_radius_ < 0.f) {
    throw std::invalid_argument("Expect search radius to be nonnegative");
  }

#ifndef NDEBUG
  for (size_t i = 0; i <= 180; ++i) {
    assert(!turn_cost_table_[i]);
  }
#endif

  if (0.f < turn_penalty_factor_) {
    for (int i = 0; i <= 180; ++i) {
      turn_cost_table_[i] = turn_penalty_factor_ * std::exp(-i/45.f);
    }
  } else if (turn_penalty_factor_ < 0.f) {
    throw std::invalid_argument("Expect turn penalty factor to be nonnegative");
  }
}


MapMatching::MapMatching(baldr::GraphReader& graphreader,
                         const sif::cost_ptr_t* mode_costing,
                         const sif::TravelMode mode,
                         const ptree& config)
    : MapMatching(graphreader, mode_costing, mode,
                  config.get<float>("sigma_z"),
                  config.get<float>("beta"),
                  config.get<float>("breakage_distance"),
                  config.get<float>("max_route_distance_factor"),
                  config.get<float>("search_radius"),
                  config.get<float>("turn_penalty_factor")) {}


MapMatching::~MapMatching()
{ Clear(); }


void
MapMatching::Clear()
{
  measurements_.clear();
  states_.clear();
  ViterbiSearch<State>::Clear();
}


template <typename candidate_iterator_t>
Time MapMatching::AppendState(const Measurement& measurement,
                              candidate_iterator_t begin,
                              candidate_iterator_t end)
{
  Time time = states_.size();

  // Append to base class
  std::vector<const State*> column;
  for (auto it = begin; it != end; it++) {
    StateId id = state_.size();
    state_.push_back(new State(id, time, *it));
    column.push_back(state_.back());
  }
  unreached_states_.push_back(column);

  states_.push_back(column);
  measurements_.push_back(measurement);

  return time;
}


inline float
MapMatching::MaxRouteDistance(const State& left, const State& right) const
{
  auto mmt_distance = GreatCircleDistance(measurement(left), measurement(right));
  return std::min(mmt_distance * max_route_distance_factor_, breakage_distance_);
}


float
MapMatching::TransitionCost(const State& left, const State& right) const
{
  if (!left.routed()) {
    std::shared_ptr<const sif::EdgeLabel> edgelabel;
    const auto prev_stateid = predecessor(left.id());
    if (prev_stateid != kInvalidStateId) {
      const auto& prev_state = state(prev_stateid);
      assert(prev_state.routed());
      const auto label = prev_state.last_label(left);
      edgelabel = label? label->edgelabel : nullptr;
    } else {
      edgelabel = nullptr;
    }
    const midgard::DistanceApproximator approximator(measurement(right).lnglat());
    left.route(unreached_states_[right.time()], graphreader_,
               MaxRouteDistance(left, right),
               approximator, search_radius_,
               costing(), edgelabel, turn_cost_table_);
  }
  assert(left.routed());

  const auto label = left.last_label(right);
  if (label) {
    const auto mmt_distance = GreatCircleDistance(measurement(left), measurement(right));
    return (label->turn_cost + std::abs(label->cost - mmt_distance)) * inv_beta_;
  }

  assert(IsInvalidCost(-1.f));
  return -1.f;
}


inline float
MapMatching::EmissionCost(const State& state) const
{ return state.candidate().sq_distance() * inv_double_sq_sigma_z_; }


inline double
MapMatching::CostSofar(double prev_costsofar, float transition_cost, float emission_cost) const
{ return prev_costsofar + transition_cost + emission_cost; }


EdgeSegment::EdgeSegment(baldr::GraphId the_edgeid,
                         float the_source,
                         float the_target)
    : edgeid(the_edgeid),
      source(the_source),
      target(the_target)
{
  if (!(0.f <= source && source <= target && target <= 1.f)) {
    throw std::invalid_argument("Expect 0.f <= source <= source <= 1.f, but you got source = "
                                + std::to_string(source)
                                + " and target = "
                                + std::to_string(target));
  }
}


std::vector<midgard::PointLL>
EdgeSegment::Shape(baldr::GraphReader& graphreader) const
{
  const baldr::GraphTile* tile = nullptr;
  const auto edge = helpers::edge_directededge(graphreader, edgeid, tile);
  if (edge) {
    const auto edgeinfo = tile->edgeinfo(edge->edgeinfo_offset());
    const auto& shape = edgeinfo->shape();
    if (edge->forward()) {
      return helpers::ClipLineString(shape.cbegin(), shape.cend(), source, target);
    } else {
      return helpers::ClipLineString(shape.crbegin(), shape.crend(), source, target);
    }
  }

  return {};
}


bool EdgeSegment::Adjoined(baldr::GraphReader& graphreader, const EdgeSegment& other) const
{
  if (edgeid != other.edgeid) {
    if (target == 1.f && other.source == 0.f) {
      const auto endnode = helpers::edge_endnodeid(graphreader, edgeid);
      return endnode == helpers::edge_startnodeid(graphreader, other.edgeid)
          && endnode.Is_Valid();
    } else {
      return false;
    }
  } else {
    return target == other.source;
  }
}


// Collect a nodeid set of a path location
std::unordered_set<baldr::GraphId>
collect_nodes(baldr::GraphReader& graphreader, const Candidate& location)
{
  std::unordered_set<baldr::GraphId> results;
  const baldr::GraphTile* tile = nullptr;

  for (const auto& edge : location.edges()) {
    if (!edge.id.Is_Valid()) continue;
    if (edge.dist == 0.f) {
      const auto& startnodeid = helpers::edge_startnodeid(graphreader, edge.id, tile);
      if (startnodeid.Is_Valid()) {
        results.insert(startnodeid);
      }
    } else if (edge.dist == 1.f) {
      const auto& endnodeid = helpers::edge_endnodeid(graphreader, edge.id, tile);
      if (endnodeid.Is_Valid()) {
        results.insert(endnodeid);
      }
    }
  }

  return results;
}


MatchResult
guess_source_result(const MapMatching::state_iterator source,
                    const MapMatching::state_iterator target,
                    const Measurement& source_measurement)
{
  if (source.IsValid() && target.IsValid()) {
    baldr::GraphId last_valid_id;
    GraphType last_valid_type = GraphType::kUnknown;
    for (auto label = source->RouteBegin(*target);
         label != source->RouteEnd(); label++) {
      if (label->nodeid.Is_Valid()) {
        last_valid_id = label->nodeid;
        last_valid_type = GraphType::kNode;
      } else if (label->edgeid.Is_Valid()) {
        last_valid_id = label->edgeid;
        last_valid_type = GraphType::kEdge;
      }
    }
    const auto& c = source->candidate();
    return {c.vertex(), c.distance(), last_valid_id, last_valid_type, &(*source)};
  } else if (source.IsValid()) {
    return {source_measurement.lnglat(), 0.f, baldr::GraphId(), GraphType::kUnknown, &(*source)};
  }

  return {source_measurement.lnglat()};
}


MatchResult
guess_target_result(const MapMatching::state_iterator source,
                    const MapMatching::state_iterator target,
                    const Measurement& target_measurement)
{
  if (source.IsValid() && target.IsValid()) {
    auto label = source->RouteBegin(*target);
    baldr::GraphId graphid;
    GraphType graphtype = GraphType::kUnknown;
    if (label != source->RouteEnd()) {
      if (label->nodeid.Is_Valid()) {
        graphid = label->nodeid;
        graphtype = GraphType::kNode;
      } else if (label->edgeid.Is_Valid()) {
        graphid = label->edgeid;
        graphtype = GraphType::kEdge;
      }
    }
    const auto& c = target->candidate();
    return {c.vertex(), c.distance(), graphid, graphtype, &(*target)};
  } else if (target.IsValid()) {
    return {target_measurement.lnglat(), 0.f, baldr::GraphId(), GraphType::kUnknown, &(*target)};
  }

  return {target_measurement.lnglat()};
}


template <typename candidate_iterator_t>
MatchResult
interpolate(baldr::GraphReader& reader,
            const std::unordered_set<baldr::GraphId>& graphset,
            candidate_iterator_t begin,
            candidate_iterator_t end,
            const Measurement& measurement)
{
  auto closest_candidate = end;
  float closest_sq_distance = std::numeric_limits<float>::infinity();
  baldr::GraphId closest_graphid;
  GraphType closest_graphtype = GraphType::kUnknown;

  for (auto candidate = begin; candidate != end; candidate++) {
    if (candidate->sq_distance() < closest_sq_distance) {
      if (!candidate->IsNode()) {
        for (const auto& edge : candidate->edges()) {
          const auto it = graphset.find(edge.id);
          if (it != graphset.end()) {
            closest_candidate = candidate;
            closest_sq_distance = candidate->sq_distance();
            closest_graphid = edge.id;
            closest_graphtype = GraphType::kEdge;
          }
        }
      } else {
        for (const auto nodeid : collect_nodes(reader, *candidate)) {
          const auto it = graphset.find(nodeid);
          if (it != graphset.end()) {
            closest_candidate = candidate;
            closest_sq_distance = candidate->sq_distance();
            closest_graphid = nodeid;
            closest_graphtype = GraphType::kNode;
          }
        }
      }
    }
  }

  if (closest_candidate != end) {
    return {closest_candidate->vertex(), closest_candidate->distance(), closest_graphid, closest_graphtype};
  }

  return {measurement.lnglat()};
}


std::unordered_set<baldr::GraphId>
collect_graphset(baldr::GraphReader& reader,
                 const MapMatching::state_iterator source,
                 const MapMatching::state_iterator target)
{
  std::unordered_set<baldr::GraphId> graphset;
  if (source.IsValid() && target.IsValid()) {
    for (auto label = source->RouteBegin(*target);
         label != source->RouteEnd();
         ++label) {
      if (label->edgeid.Is_Valid()) {
        graphset.insert(label->edgeid);
      }
      if (label->nodeid.Is_Valid()) {
        graphset.insert(label->nodeid);
      }
    }
  } else if (source.IsValid()) {
    const auto& location = source->candidate();
    if (!location.IsNode()) {
      for (const auto& edge : location.edges()) {
        if (edge.id.Is_Valid()) {
          graphset.insert(edge.id);
        }
      }
    } else {
      for (const auto nodeid : collect_nodes(reader, location)) {
        if (nodeid.Is_Valid()) {
          graphset.insert(nodeid);
        }
      }
    }
  }

  return graphset;
}


std::vector<MatchResult>
OfflineMatch(MapMatching& mm,
             const CandidateQuery& cq,
             const std::vector<Measurement>& measurements,
             float sq_search_radius,
             float interpolation_distance)
{
  mm.Clear();

  if (measurements.empty()) {
    return {};
  }

  using mmt_size_t = std::vector<Measurement>::size_type;
  Time time = 0;
  float sq_interpolation_distance = interpolation_distance * interpolation_distance;
  std::unordered_map<Time, std::vector<mmt_size_t>> proximate_measurements;

  // Load states
  for (mmt_size_t idx = 0,
             last_idx = 0,
              end_idx = measurements.size() - 1;
       idx <= end_idx; idx++) {
    const auto& measurement = measurements[idx];
    auto sq_distance = GreatCircleDistanceSquared(measurements[last_idx], measurement);
    // Always match the first and the last measurement
    if (sq_interpolation_distance <= sq_distance || idx == 0 || idx == end_idx) {
      const auto& candidates = cq.Query(measurement.lnglat(),
                                        sq_search_radius,
                                        mm.costing()->GetFilter());
      time = mm.AppendState(measurement, candidates.begin(), candidates.end());
      last_idx = idx;
    } else {
      proximate_measurements[time].push_back(idx);
    }
  }

  // Search viterbi path
  std::vector<MapMatching::state_iterator> iterpath;
  iterpath.reserve(mm.size());
  for (auto it = mm.SearchPath(time); it != mm.PathEnd(); it++) {
    iterpath.push_back(it);
  }
  std::reverse(iterpath.begin(), iterpath.end());
  assert(iterpath.size() == mm.size());

  // Interpolate proximate measurements and merge their states into
  // the results
  std::vector<MatchResult> results;
  results.reserve(measurements.size());
  results.emplace_back(measurements.front().lnglat());
  assert(!results.back().graphid().Is_Valid());

  for (Time time = 1; time < mm.size(); time++) {
    const auto &source_state = iterpath[time - 1],
               &target_state = iterpath[time];

    if (!results.back().graphid().Is_Valid()) {
      results.pop_back();
      results.push_back(guess_source_result(source_state, target_state, measurements[results.size()]));
    }

    auto it = proximate_measurements.find(time - 1);
    if (it != proximate_measurements.end()) {
      const auto& graphset = collect_graphset(mm.graphreader(), source_state, target_state);
      for (const auto idx : it->second) {
        const auto& candidates = cq.Query(measurements[idx].lnglat(),
                                          sq_search_radius,
                                          mm.costing()->GetFilter());
        results.push_back(interpolate(mm.graphreader(), graphset,
                                      candidates.begin(), candidates.end(),
                                      measurements[idx]));
      }
    }

    results.push_back(guess_target_result(source_state, target_state, measurements[results.size()]));
  }
  assert(results.size() == measurements.size());

  return results;
}


MapMatcher::MapMatcher(const ptree& config,
                       baldr::GraphReader& graphreader,
                       CandidateGridQuery& rangequery,
                       const sif::cost_ptr_t* mode_costing,
                       sif::TravelMode travelmode)
    : config_(config),
      graphreader_(graphreader),
      rangequery_(rangequery),
      mode_costing_(mode_costing),
      travelmode_(travelmode),
      mapmatching_(graphreader_, mode_costing_, travelmode_, config_) {}


MapMatcher::~MapMatcher() {}


std::vector<MatchResult>
MapMatcher::OfflineMatch(const std::vector<Measurement>& measurements)
{
  float search_radius = std::min(config_.get<float>("search_radius"),
                                 config_.get<float>("max_search_radius"));
  float interpolation_distance = config_.get<float>("interpolation_distance");
  return mmp::OfflineMatch(mapmatching_, rangequery_, measurements,
                           search_radius * search_radius,
                           interpolation_distance);
}


MapMatcherFactory::MapMatcherFactory(const ptree& root)
    : config_(root.get_child("mm")),
      graphreader_(root.get_child("mjolnir")),
      mode_costing_{nullptr},
      mode_name_(),
      rangequery_(graphreader_,
                  local_tile_size(graphreader_)/root.get<size_t>("grid.size"),
                  local_tile_size(graphreader_)/root.get<size_t>("grid.size")),
      max_grid_cache_size_(root.get<float>("grid.cache_size"))
      {
#ifndef NDEBUG
        for (size_t idx = 0; idx < kModeCostingCount; idx++) {
          assert(!mode_costing_[idx]);
          assert(mode_name_[idx].empty());
        }
#endif

        init_costings(root);
      }


MapMatcherFactory::~MapMatcherFactory() {}


sif::TravelMode
MapMatcherFactory::NameToTravelMode(const std::string& name)
{
  for (size_t idx = 0; idx < kModeCostingCount; idx++) {
    if (!name.empty() && mode_name_[idx] == name) {
      return static_cast<sif::TravelMode>(idx);
    }
  }
  throw std::invalid_argument("Invalid costing name: " + name);
}


const std::string&
MapMatcherFactory::TravelModeToName(sif::TravelMode travelmode)
{
  const auto index = static_cast<size_t>(travelmode);
  if (index < kModeCostingCount) {
    if (!mode_name_[index].empty()) {
      return mode_name_[index];
    }
  }
  throw std::invalid_argument("Invalid travelmode code " + std::to_string(index));
}


MapMatcher*
MapMatcherFactory::Create(const ptree& preferences)
{
  const auto& name = preferences.get<std::string>("mode", config_.get<std::string>("mode"));
  auto travelmode = NameToTravelMode(name);
  const auto& config = MergeConfig(name, preferences);
  return Create(travelmode, preferences);
}


MapMatcher*
MapMatcherFactory::Create(sif::TravelMode travelmode, const ptree& preferences)
{
  const auto& config = MergeConfig(TravelModeToName(travelmode), preferences);
  // TODO investigate exception safety
  return new MapMatcher(config, graphreader_, rangequery_, mode_costing_, travelmode);
}


ptree
MapMatcherFactory::MergeConfig(const std::string& name, const ptree& preferences)
{
  // Copy the default child config
  auto config = config_.get_child("default");

  // The mode-specific config overwrites defaults
  const auto mode_config = config_.get_child_optional(name);
  if (mode_config) {
    for (const auto& child : *mode_config) {
      config.put_child(child.first, child.second);
    }
  }

  // Preferences overwrites defaults
  for (const auto& child : preferences) {
    config.put_child(child.first, child.second);
  }

  // Give it back
  return config;
}


ptree&
MapMatcherFactory::MergeConfig(const std::string& name, ptree& preferences)
{
  const auto mode_config = config_.get_child_optional(name);
  if (mode_config) {
    for (const auto& child : *mode_config) {
      auto pchild = preferences.get_child_optional(child.first);
      if (!pchild) {
        preferences.put_child(child.first, child.second);
      }
    }
  }

  for (const auto& child : config_.get_child("default")) {
    auto pchild = preferences.get_child_optional(child.first);
    if (!pchild) {
      preferences.put_child(child.first, child.second);
    }
  }

  return preferences;
}


size_t
MapMatcherFactory::register_costing(const std::string& mode_name,
                                    factory_function_t factory,
                                    const ptree& config)
{
  auto costing = factory(config);
  auto index = static_cast<size_t>(costing->travelmode());
  if (!(index < kModeCostingCount)) {
    throw std::out_of_range("Configuration error: out of bounds");
  }
  if (mode_costing_[index]) {
    throw std::runtime_error("Configuration error: found duplicate travel mode");
  }
  mode_costing_[index] = costing;
  mode_name_[index] = mode_name;
  return index;
}


sif::cost_ptr_t*
MapMatcherFactory::init_costings(const ptree& root)
{
  register_costing("auto", sif::CreateAutoCost, root.get_child("costing_options.auto"));
  register_costing("bicycle", sif::CreateBicycleCost, root.get_child("costing_options.bicycle"));
  register_costing("pedestrian", sif::CreatePedestrianCost, root.get_child("costing_options.pedestrian"));
  register_costing("multimodal", CreateUniversalCost, root.get_child("costing_options.multimodal"));

  return mode_costing_;
}


void MapMatcherFactory::ClearFullCache()
{
  if(graphreader_.OverCommitted()) {
    graphreader_.Clear();
  }

  if (rangequery_.size() > max_grid_cache_size_) {
    rangequery_.Clear();
  }
}


void MapMatcherFactory::ClearCache()
{
  graphreader_.Clear();
  rangequery_.Clear();
}


}
