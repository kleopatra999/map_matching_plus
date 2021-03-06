#include <vector>
#include <unordered_set>
#include <unordered_map>

#include <valhalla/midgard/distanceapproximator.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/pathlocation.h>
#include <valhalla/sif/dynamiccost.h>
#include <valhalla/sif/costconstants.h>

#include "mmp/graph_helpers.h"
#include "mmp/geometry_helpers.h"
#include "mmp/routing.h"

using namespace valhalla;


namespace mmp
{

LabelSet::LabelSet(typename BucketQueue<uint32_t, kInvalidLabelIndex>::size_type count, float size)
    : queue_(count, size) {}


bool
LabelSet::put(const baldr::GraphId& nodeid, sif::TravelMode travelmode,
              std::shared_ptr<const sif::EdgeLabel> edgelabel)
{
  return put(nodeid, {},         // nodeid, (invalid) edgeid
             0.f, 0.f,           // source, target
             0.f, 0.f, 0.f,      // cost, turn cost, sort cost
             kInvalidLabelIndex, // predecessor
             nullptr, travelmode, edgelabel);
}


bool
LabelSet::put(const baldr::GraphId& nodeid,
              const baldr::GraphId& edgeid,
              float source, float target,
              float cost, float turn_cost, float sortcost,
              uint32_t predecessor,
              const baldr::DirectedEdge* edge,
              sif::TravelMode travelmode,
              std::shared_ptr<const sif::EdgeLabel> edgelabel)
{
  if (!nodeid.Is_Valid()) {
    throw std::runtime_error("invalid nodeid");
  }
  const auto it = node_status_.find(nodeid);
  if (it == node_status_.end()) {
    uint32_t idx = labels_.size();
    bool added = queue_.add(idx, sortcost);
    if (added) {
      labels_.emplace_back(nodeid, edgeid,
                           source, target,
                           cost, turn_cost, sortcost,
                           predecessor,
                           edge, travelmode, edgelabel);
      node_status_[nodeid] = {idx, false};
      return true;
    }
    // !added -> rejected since queue's full
  } else {
    const auto& status = it->second;
    if (!status.permanent && sortcost < labels_[status.label_idx].sortcost) {
      // TODO check if it goes through constructor
      labels_[status.label_idx] = {nodeid, edgeid,
                                   source, target,
                                   cost, turn_cost, sortcost,
                                   predecessor,
                                   edge, travelmode, edgelabel};
      bool decreased = queue_.decrease(status.label_idx, sortcost);
      assert(decreased);
      return true;
    }
  }

  return false;
}


bool
LabelSet::put(uint16_t dest, sif::TravelMode travelmode,
              std::shared_ptr<const sif::EdgeLabel> edgelabel)
{
  return put(dest, {},           // dest, (invalid) edgeid
             0.f, 0.f,           // source, target
             0.f, 0.f, 0.f,      // cost, turn cost, sort cost
             kInvalidLabelIndex, // predecessor
             nullptr, travelmode, edgelabel);
}


bool
LabelSet::put(uint16_t dest,
              const baldr::GraphId& edgeid,
              float source, float target,
              float cost, float turn_cost, float sortcost,
              uint32_t predecessor,
              const baldr::DirectedEdge* edge,
              sif::TravelMode travelmode,
              std::shared_ptr<const sif::EdgeLabel> edgelabel)
{
  if (dest == kInvalidDestination) {
    throw std::runtime_error("invalid destination");
  }

  const auto it = dest_status_.find(dest);
  if (it == dest_status_.end()) {
    uint32_t idx = labels_.size();
    bool added = queue_.add(idx, sortcost);
    if (added) {
      labels_.emplace_back(dest, edgeid,
                           source, target,
                           cost, turn_cost, sortcost,
                           predecessor,
                           edge, travelmode, edgelabel);
      dest_status_[dest] = {idx, false};
      return true;
    }
    // !added -> rejected since queue's full
  } else {
    const auto& status = it->second;
    if (!status.permanent && sortcost < labels_[status.label_idx].sortcost) {
      // TODO check if it goes through constructor
      labels_[status.label_idx] = {dest, edgeid,
                                   source, target,
                                   cost, turn_cost, sortcost,
                                   predecessor,
                                   edge, travelmode, edgelabel};
      bool decreased = queue_.decrease(status.label_idx, sortcost);
      assert(decreased);
      return true;
    }
  }

  return false;
}


uint32_t
LabelSet::pop()
{
  const auto idx = queue_.pop();

  if (idx != kInvalidLabelIndex) {
    const auto& label = labels_[idx];
    if (label.nodeid.Is_Valid()) {
      assert(node_status_[label.nodeid].label_idx == idx);
      assert(!node_status_[label.nodeid].permanent);
      node_status_[label.nodeid].permanent = true;
    } else {
      assert(dest_status_[label.dest].label_idx == idx);
      assert(!dest_status_[label.dest].permanent);
      assert(label.dest != kInvalidDestination);
      dest_status_[label.dest].permanent = true;
    }
  }

  return idx;
}


inline bool
IsEdgeAllowed(const baldr::DirectedEdge* edge,
              const baldr::GraphId& edgeid,
              const sif::cost_ptr_t costing,
              const std::shared_ptr<const sif::EdgeLabel> pred_edgelabel,
              const sif::EdgeFilter edgefilter,
              const baldr::GraphTile* tile)
{
  if (costing) {
    if (pred_edgelabel) {
      // Still on the same edge and the predecessor's show-up here
      // means it was allowed so we give it a pass directly

      // TODO let sif do this?
      return edgeid == pred_edgelabel->edgeid()
          || costing->Allowed(edge, *pred_edgelabel, tile, edgeid);
    } else {
      if (edgefilter) {
        return !edgefilter(edge);
      } else {
        return true;
      }
    }
  } else {
    return true;
  }
}


void set_origin(baldr::GraphReader& reader,
                const std::vector<baldr::PathLocation>& destinations,
                uint16_t origin_idx,
                LabelSet& labelset,
                const sif::TravelMode travelmode,
                sif::cost_ptr_t costing,
                std::shared_ptr<const sif::EdgeLabel> edgelabel)
{
  const baldr::GraphTile* tile = nullptr;

  for (const auto& edge : destinations[origin_idx].edges()) {
    assert(edge.id.Is_Valid());
    if (!edge.id.Is_Valid()) continue;

    if (edge.dist == 0.f) {
      const auto nodeid = helpers::edge_startnodeid(reader, edge.id, tile);
      if (!nodeid.Is_Valid()) continue;

#if 1  // TODO perhaps we shouldn't check start node
      const auto nodeinfo = helpers::edge_nodeinfo(reader, nodeid, tile);
      if (!nodeinfo) continue;
      if (costing && !costing->Allowed(nodeinfo)) continue;
#endif

      labelset.put(nodeid, travelmode, edgelabel);
    } else if (edge.dist == 1.f) {
      const auto nodeid = helpers::edge_endnodeid(reader, edge.id, tile);
      if (!nodeid.Is_Valid()) continue;

#if 1  // TODO perhaps we shouldn't check start node
      const auto nodeinfo = helpers::edge_nodeinfo(reader, nodeid, tile);
      if (!nodeinfo) continue;
      if (costing && !costing->Allowed(nodeinfo)) continue;
#endif

      labelset.put(nodeid, travelmode, edgelabel);
    } else {
      assert(0.f < edge.dist && edge.dist < 1.f);
      // Will decide whether to filter out this edge later
      labelset.put(origin_idx, travelmode, edgelabel);
    }
  }
}


void set_destinations(baldr::GraphReader& reader,
                      const std::vector<baldr::PathLocation>& destinations,
                      std::unordered_map<baldr::GraphId, std::unordered_set<uint16_t>>& node_dests,
                      std::unordered_map<baldr::GraphId, std::unordered_set<uint16_t>>& edge_dests)
{
  const baldr::GraphTile* tile = nullptr;

  for (uint16_t dest = 0; dest < destinations.size(); dest++) {
    for (const auto& edge : destinations[dest].edges()) {
      assert(edge.id.Is_Valid());
      if (!edge.id.Is_Valid()) continue;

      if (edge.dist == 0.f) {
        const auto nodeid = helpers::edge_startnodeid(reader, edge.id, tile);
        if (!nodeid.Is_Valid()) continue;
        node_dests[nodeid].insert(dest);

      } else if (edge.dist == 1.f) {
        const auto nodeid = helpers::edge_endnodeid(reader, edge.id, tile);
        if (!nodeid.Is_Valid()) continue;
        node_dests[nodeid].insert(dest);

      } else {
        edge_dests[edge.id].insert(dest);
      }
    }
  }
}


inline uint16_t
get_inbound_edgelabel_heading(baldr::GraphReader& graphreader,
                              const baldr::GraphTile* tile,
                              const sif::EdgeLabel& edgelabel,
                              const baldr::NodeInfo& nodeinfo)
{
  const auto idx = edgelabel.opp_local_idx();
  if (idx < 8) {
    return nodeinfo.heading(idx);
  } else {
    const auto directededge = helpers::edge_directededge(graphreader, edgelabel.edgeid(), tile);
    const auto edgeinfo = tile->edgeinfo(directededge->edgeinfo_offset());
    const auto& shape = edgeinfo->shape();
    if (shape.size() >= 2) {
      float heading;
      if (directededge->forward()) {
        heading = shape.back().Heading(shape.rbegin()[1]);
      } else {
        heading = shape.front().Heading(shape[1]);
      }
      return static_cast<uint16_t>(std::max(0.f, std::min(359.f, heading)));
    } else {
      return 0;
    }
  }
}


inline uint16_t
get_outbound_edge_heading(const baldr::GraphTile* tile,
                          const baldr::DirectedEdge* outbound_edge,
                          const baldr::NodeInfo& nodeinfo)
{
  const auto idx = outbound_edge->localedgeidx();
  if (idx < 8) {
    return nodeinfo.heading(idx);
  } else {
    const auto edgeinfo = tile->edgeinfo(outbound_edge->edgeinfo_offset());
    const auto& shape = edgeinfo->shape();
    if (shape.size() >= 2) {
      float heading;
      if (outbound_edge->forward()) {
        heading = shape.front().Heading(shape[1]);
      } else {
        heading = shape.back().Heading(shape.rbegin()[1]);
      }
      return static_cast<uint16_t>(std::max(0.f, std::min(359.f, heading)));
    } else {
      return 0;
    }
  }
}


inline float
heuristic(const midgard::DistanceApproximator& approximator,
          const PointLL& lnglat,
          float search_radius)
{
  const auto distance = sqrtf(approximator.DistanceSquared(lnglat));
  return std::max(0.f, distance - search_radius);
}


std::unordered_map<uint16_t, uint32_t>
find_shortest_path(baldr::GraphReader& reader,
                   const std::vector<baldr::PathLocation>& destinations,
                   uint16_t origin_idx,
                   LabelSet& labelset,
                   const midgard::DistanceApproximator& approximator,
                   float search_radius,
                   sif::cost_ptr_t costing,
                   std::shared_ptr<const sif::EdgeLabel> edgelabel,
                   const float turn_cost_table[181])
{
  // Destinations at nodes
  std::unordered_map<baldr::GraphId, std::unordered_set<uint16_t>> node_dests;

  // Destinations along edges
  std::unordered_map<baldr::GraphId, std::unordered_set<uint16_t>> edge_dests;

  // Load destinations
  set_destinations(reader, destinations, node_dests, edge_dests);

  const sif::TravelMode travelmode = costing? costing->travelmode() : static_cast<sif::TravelMode>(0);

  // Load origin to the queue of the labelset
  set_origin(reader, destinations, origin_idx, labelset, travelmode, costing, edgelabel);

  std::unordered_map<uint16_t, uint32_t> results;

  const auto edgefilter = costing? costing->GetFilter() : nullptr;

  const baldr::GraphTile* tile = nullptr;

  while (!labelset.empty()) {
    const auto label_idx = labelset.pop();
    // NOTE this refernce is possible to be invalid when you add
    // labels to the set later (which causes the label list
    // reallocated)
    const auto& label = labelset.label(label_idx);

    // So we cache the costs that will be used during expanding
    const auto label_cost = label.cost;
    const auto label_turn_cost = label.turn_cost;
    // and edgelabel pointer for checking edge accessibility later
    const auto pred_edgelabel = label.edgelabel;

    if (label.nodeid.Is_Valid()) {
      const auto nodeid = label.nodeid;

      // If this node is a destination, path to destinations at this
      // node is found: remember them and remove this node from the
      // destination list
      const auto it = node_dests.find(nodeid);
      if (it != node_dests.end()) {
        for (const auto dest : it->second) {
          results[dest] = label_idx;
        }
        node_dests.erase(it);
      }

      // Congrats!
      if (node_dests.empty() && edge_dests.empty()) {
        break;
      }

      const auto nodeinfo = helpers::edge_nodeinfo(reader, nodeid, tile);
      if (!nodeinfo || nodeinfo->edge_count() <= 0) continue;

      if (costing && !costing->Allowed(nodeinfo)) continue;

      const auto inbound_heading = (pred_edgelabel && turn_cost_table)?
                                   get_inbound_edgelabel_heading(reader, tile, *pred_edgelabel, *nodeinfo) : 0;
      assert(0 <= inbound_heading && inbound_heading < 360);

      // Expand current node
      baldr::GraphId other_edgeid(nodeid.tileid(), nodeid.level(), nodeinfo->edge_index());
      auto other_edge = tile->directededge(nodeinfo->edge_index());
      assert(other_edge);
      for (size_t i = 0; i < nodeinfo->edge_count(); i++, other_edge++, other_edgeid++) {
        // Disable shortcut TODO perhaps we should use
        // other_edge->is_shortcut()? but it failed to guarantee same
        // level
        if (nodeid.level() != other_edge->endnode().level()) continue;

        if (!IsEdgeAllowed(other_edge, other_edgeid, costing, pred_edgelabel, edgefilter, tile)) continue;

        // Turn cost
        float turn_cost = 0.f;
        if (pred_edgelabel && turn_cost_table) {
          const auto other_heading = get_outbound_edge_heading(tile, other_edge, *nodeinfo);
          assert(0 <= other_heading && other_heading < 360);
          const auto turn_degree = helpers::get_turn_degree180(inbound_heading, other_heading);
          assert(0 <= turn_degree && turn_degree <= 180);
          turn_cost = label_turn_cost + turn_cost_table[turn_degree];
        }

        // If destinations found along the edge, add segments to each
        // destination to the queue
        const auto it = edge_dests.find(other_edgeid);
        if (it != edge_dests.end()) {
          for (const auto dest : it->second) {
            for (const auto& edge : destinations[dest].edges()) {
              if (edge.id == other_edgeid) {
                const float cost = label_cost + other_edge->length() * edge.dist,
                        sortcost = cost;
                labelset.put(dest, other_edgeid,
                             0.f, edge.dist,
                             cost, turn_cost, sortcost,
                             label_idx,
                             other_edge, travelmode, nullptr);
              }
            }
          }
        }

        const baldr::GraphTile* endtile = tile;
        if (other_edge->endnode().tileid() != tile->id().tileid()) {
          endtile = reader.GetGraphTile(other_edge->endnode());
        }
        const auto other_nodeinfo = endtile->node(other_edge->endnode());
        const float cost = label_cost + other_edge->length(),
                sortcost = cost + heuristic(approximator, other_nodeinfo->latlng(), search_radius);
        labelset.put(other_edge->endnode(), other_edgeid,
                     0.f, 1.f,
                     cost, turn_cost, sortcost,
                     label_idx,
                     other_edge, travelmode, nullptr);
      }
    } else {
      assert(label.dest != kInvalidDestination);
      const auto dest = label.dest;

      // Path to a destination along an edge is found: remember it and
      // remove the destination from the destination list
      results[dest] = label_idx;
      for (const auto& edge : destinations[dest].edges()) {
        const auto it = edge_dests.find(edge.id);
        if (it != edge_dests.end()) {
          it->second.erase(dest);
          if (it->second.empty()) {
            edge_dests.erase(it);
          }
        }
      }

      // Congrats!
      if (edge_dests.empty() && node_dests.empty()) {
        break;
      }

      // Expand origin: add segments from origin to destinations ahead
      // at the same edge to the queue
      if (dest == origin_idx) {
        for (const auto& origin_edge : destinations[origin_idx].edges()) {
          const auto directededge = helpers::edge_directededge(reader, origin_edge.id, tile);
          if (!directededge) continue;

          if (!IsEdgeAllowed(directededge, origin_edge.id, costing, pred_edgelabel, edgefilter, tile)) continue;

          // U-turn cost
          float turn_cost = 0.f;
          if (pred_edgelabel && turn_cost_table
              && pred_edgelabel->edgeid() != origin_edge.id
              && pred_edgelabel->opp_local_idx() == directededge->localedgeidx()) {
            turn_cost = label_turn_cost + turn_cost_table[0];
          }

          // All destinations on this origin edge
          for (const auto other_dest : edge_dests[origin_edge.id]) {
            // All edges of this destination
            for (const auto& other_edge : destinations[other_dest].edges()) {
              if (origin_edge.id == other_edge.id && origin_edge.dist <= other_edge.dist) {
                const float cost = label_cost + directededge->length() * (other_edge.dist - origin_edge.dist),
                        sortcost = cost;
                labelset.put(other_dest, origin_edge.id,
                             origin_edge.dist, other_edge.dist,
                             cost, turn_cost, sortcost,
                             label_idx,
                             directededge, travelmode, nullptr);
              }
            }
          }

          const baldr::GraphTile* endtile = tile;
          if (directededge->endnode().tileid() != tile->id().tileid()) {
            endtile = reader.GetGraphTile(directededge->endnode());
          }
          const auto nodeinfo = endtile->node(directededge->endnode());
          const float cost = label_cost + directededge->length() * (1.f - origin_edge.dist),
                  sortcost = cost + heuristic(approximator, nodeinfo->latlng(), search_radius);
          labelset.put(directededge->endnode(), origin_edge.id,
                       origin_edge.dist, 1.f,
                       cost, turn_cost, sortcost,
                       label_idx,
                       directededge, travelmode, nullptr);
        }
      }
    }
  }

  labelset.clear_queue();
  labelset.clear_status();

  return results;
}

}
