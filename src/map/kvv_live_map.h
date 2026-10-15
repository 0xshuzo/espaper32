#pragma once

#include <optional>
#include <string>
#include <vector>
#include "rail_geometry.h"
#include "kvv_efa_query.h"
#include "map_config.h"

class MonochromeCanvas;

namespace kvv {

using BuildingOutlines = std::vector<std::vector<GeoPoint>>;

// Presentation distinguishes streets from walking/cycling paths without
// introducing display styling into the OSM query library.
struct Highway {
    std::vector<GeoPoint> points;
    bool footpath = false;
};

// Prepared once per OSM result, then reused for every transit redraw.
struct MapBackground {
    explicit MapBackground(const osm::MapData& data);
    MapBackground() = default;

    BuildingOutlines buildings;
    std::vector<Stop> stops;
    RailGeometry rails;
    std::vector<Highway> highways;
};

// Geographic bounds calculated from the same projection as the renderer.
osm::BoundingBox MapViewport(const MapConfig& config = {});

// Select the closest named station by geographic distance, regardless of trips.
std::optional<Stop> NearestStation(const std::vector<Stop>& stops, GeoPoint center);

// A transit estimate after it has been matched to the local rail graph.
struct Vehicle {
    std::string line;
    std::string destination;
    GeoPoint position;
};

// Static geometry and current transit data ready for one complete redraw.
struct LiveMap {
    MapConfig config;
    std::optional<Stop> nearest_station;
    std::string server_time;
    BuildingOutlines buildings;
    std::vector<std::vector<GeoPoint>> tracks;
    std::vector<int> track_stroke_widths;
    std::vector<Stop> stops;
    std::vector<Vehicle> vehicles;
    std::vector<Highway> highways;

    bool stale = false;
    bool replay = false;
    int age_seconds = 0;
    std::string map_status;
};

// Application adapter: attaches queried map data and maps transit progress to rails.
LiveMap MakeLiveMap(
    const TransitSnapshot& snapshot, const MapBackground& background, const MapConfig& config = {});

// Request state supplements readiness, which is determined by available data.
struct MapScreenStatus {
    bool map_loading = false;
    bool map_failed = false;
    bool transit_failed = false;
};

// Keep the loading screen visible until both sources have a successful result.
// Empty results are valid, and retained data stays visible during later refreshes.
void DrawMapScreen(MonochromeCanvas& canvas, const std::optional<TransitSnapshot>& snapshot,
    const MapBackground* background, const MapScreenStatus& status = {},
    const MapConfig& config = {});

// Draws a 1000 m-wide view around the configured coordinates. The north/south
// extent follows the available height, preserving geographic proportions.
// All map layers and vehicle markers use the full rectangular viewport.
void DrawLiveMap(MonochromeCanvas& canvas, const LiveMap& live_map);
void DrawLiveMapError(MonochromeCanvas& canvas, const std::string& error);

} // namespace kvv
