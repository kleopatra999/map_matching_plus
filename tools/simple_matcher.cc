#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "mmp/measurement.h"
#include "mmp/map_matching.h"

using namespace mmp;


int main(int argc, char *argv[])
{
  if (argc < 2) {
    std::cout << "usage: map_matching CONFIG" << std::endl;
    return 1;
  }

  boost::property_tree::ptree config;
  boost::property_tree::read_json(argv[1], config);

  const float default_gps_accuracy = config.get<float>("mm.gps_accuracy"),
             default_search_radius = config.get<float>("mm.search_radius");
  const std::string modename = config.get<std::string>("mm.mode");

  MapMatcherFactory matcher_factory(config);
  auto matcher = matcher_factory.Create(modename);

  std::vector<Measurement> measurements;
  std::vector<std::string> uuids;
  std::string line;

  size_t index = 0;
  while (true) {
    std::getline(std::cin, line);
    if (std::cin.eof() || line.empty()) {

      // Offline match
      std::cout.precision(15);
      const auto& results = matcher->OfflineMatch(measurements);

      // Show results
      size_t mmt_id = 0, count = 0;
      for (const auto& result : results) {
        const auto state = result.state();
        const Measurement& measurement = measurements[mmt_id];
        const std::string& uuid = uuids[mmt_id];
        const PointLL point = measurement.lnglat();
        float originalLat = point.lat();
        float originalLon = point.lng();
        std::cout << uuid << "," << originalLat << "," << originalLon << ",";
        if (state) {
          float lon = state->candidate().vertex().lng();
          float lat = state->candidate().vertex().lat();
          std::cout << lat << "," << lon;
          count++;
        } else {
          std::cout << ",";
        }
        mmt_id++;

        std::cout << "," << default_gps_accuracy << "," << default_search_radius << std::endl;
      }

      // Clean up
      measurements.clear();
      matcher_factory.ClearFullCache();

      if (std::cin.eof()) {
        break;
      } else {
        continue;
      }
    }

    // Read coordinates from the input line
    float lng, lat;
    std::string uuid;
    std::stringstream stream(line);
    stream >> uuid;
    stream >> lng;
    stream >> lat;
    uuids.emplace_back(uuid);
    measurements.emplace_back(PointLL(lng, lat),
                              default_gps_accuracy,
                              default_search_radius);
  }

  delete matcher;
  matcher_factory.ClearCache();

  return 0;
}
