# MMP Library API

*The API is still in testing and will be changed any time. Any suggestions are welcome to share at [GitHub Issues](https://github.com/mapillary/map_matching_plus/issues).*

*`TODO` The API listed here is not complete but ready for simple use.*

## Measurement

A `Measurement` object is a measured point read from GPS devices and
it is usually inaccurate and noisy therefore needed to be
matched. Extra attributes such as accuracy and timestamp can be
optionally attached to this object to help improve matching
performance.

```C++
// Constructor
mmp::Measurement(float longitude, float latitude);
```

## Map Matcher Factory

A `MapMatcherFactory` object produces [`MapMatcher`](#map-matcher)
objects for a specific transport mode. Other than that, it also
manages in-memory data structures (e.g. tiles) shared among its
matchers for you. It is recommended to instantiate it only once; but
you have to keep it until all its matchers get destroyed.

Pass it a valid configuration object, otherwise it throws
`std::invalid_argument`.

```C++
boost::property_tree::ptree config;
boost::property_tree::json_parser read(config, "mm.json");

// Constructor
mmp::MapMatcherFactory(const boost::property_tree::ptree& config);
```

To create a `MapMatcher` object of a specific transport mode:

```C++
// Possibly throw std::invalid_argument if invalid parameters are
// found in this mode's configuration
mmp::MapMather*
mmp::MapMatcherFactory::Create(const std::string& mode_name);
```

You should take care of the raw `MapMatcher` pointer returned by the
factory.

## Map Matcher

`MapMatcher` object is responsible for matching sequences to the road
network. It is created by
[`MapMatcherFactory`](#map-matcher-factory).

To match a sequence offline:
```C++
std::vector<MatchResult>
mmp::MapMatcher::OfflineMatch(const std::vector<Measurement>& sequence);
```

It returns a list of [`MatchResult`](#match-result) objects in one to
one correspondence to the input sequence.

## Match Result

A `Match Result` object contains information about which road and
where the corresponding measurement is matched to, and how to
construct the route from previous result. It is usually generated by a
[`MapMatcher`](#map-matcher) object as one result of a sequential
matching procedure.

Here are some attributes:
```C++
// Matched coordinate
const valhalla::midgard::PointLL&
mmp::MatchResult::matched_coordinate();

// Distance from measurement to the matched coordinate
float mmp::MatchResult::distance();

// GraphId identify edges and nodes internally in Valhalla tiled data
valhalla::baldr::GraphId&
mmp::MatchResult::graphid();

// A GraphId never tells you if it is edge or node whereas this
// attribute tells you
mmp::GraphType mmp::MatchResult::graphtype();
```
