// Deterministic raster checks for the map spillover/overlap regression. Passing
// an output directory also exports BMP previews without opening a GUI window.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <nlohmann/json.hpp>

#include "kvv_live_map.h"
#include "map_config.h"
#include <kvv_efa_query.h>
#include "monochrome_canvas.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Frame = std::array<unsigned char, EPD_FRAME_BYTES>;
using Json = nlohmann::json;

static void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static kvv::GeoPoint Position(double east, double north) {
    const double longitude_scale = 111320.0 * std::cos(49.006250 * 3.141592653589793 / 180.0);
    return {49.006250 + north / 111320.0, 8.431785 + east / longitude_scale};
}

static bool Black(const Frame& frame, int x, int y) {
    return (frame[y * (EPD_WIDTH / 8) + x / 8] & (0x80 >> (x % 8))) == 0;
}

// Raster fixtures are relative to the panned viewport; transit fixtures above
// and below continue to use actual station-relative geographic coordinates.
static kvv::GeoPoint ViewPosition(double east, double north) {
    const auto center = Position(-200, 100);
    const double longitude_scale = 111320.0 * std::cos(center.latitude * 3.141592653589793 / 180.0);
    return {center.latitude + north / 111320.0, center.longitude + east / longitude_scale};
}

// Exact pixel fixtures keep new stroke probes clear of map labels.
static kvv::GeoPoint PixelPosition(double x, double y) {
    return ViewPosition((x - 400) / 0.768, (260 - y) / 0.768);
}

static kvv::MapConfig FixtureConfig() {
    kvv::MapConfig config;
    config.center = ViewPosition(0, 0);
    config.places = {{PixelPosition(415, 281), "CUSTOM PLACE LABEL"}};
    return config;
}

static Frame Render(const kvv::LiveMap& map) {
    Frame frame;
    MonochromeCanvas canvas(frame.data());
    kvv::DrawLiveMap(canvas, map);
    return frame;
}

static void Save(const Frame& frame, const std::string& path) {
    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, EPD_WIDTH, EPD_HEIGHT, 24, SDL_PIXELFORMAT_RGB24);
    Require(surface != nullptr, "Could not create preview surface");

    // Expand the native one-bit frame to an RGB surface for deterministic BMPs.
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        auto* row = static_cast<unsigned char*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < EPD_WIDTH; ++x) {
            row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = Black(frame, x, y) ? 0 : 255;
        }
    }

    const int status = SDL_SaveBMP(surface, path.c_str());
    SDL_FreeSurface(surface);
    Require(status == 0, "Could not save preview");
}

static kvv::Stop Stop(const std::string& id, double east, double north) {
    return {id, id, Position(east, north)};
}

static Json Call(const char* id, double east, double north, const char* time) {
    const auto point = Position(east, north);
    return {{"nameWO", "Tullastrasse"},
        {"ref",
            {{"id", id},
                {"coords", std::to_string(point.longitude) + "," + std::to_string(point.latitude)},
                {"depDateTimeSec", time}}}};
}

static osm::RailWay Track(const std::vector<kvv::GeoPoint>& points, const std::vector<int>& ids) {
    osm::RailWay way{ids.front(), "", {}};
    for (std::size_t i = 0; i < points.size(); ++i) {
        way.nodes.push_back({ids.at(i), {points[i].latitude, points[i].longitude}});
    }
    return way;
}

static osm::Highway MappedHighway(
    std::int64_t id, const std::string& kind, const std::vector<kvv::GeoPoint>& points) {
    osm::Highway way{id, kind, {}};
    for (const auto& point : points) {
        way.points.push_back({point.latitude, point.longitude});
    }
    return way;
}

static osm::MapData TestMapData() {
    osm::MapData data{kvv::MapViewport(FixtureConfig()), {}, {}, {}};
    data.rails = {Track({Position(-700, 0), Position(0, 0), Position(500, 0)}, {1, 2, 3})};
    const auto neighbour = Position(-500, 100);
    data.stops = {{6, "Neighbour", "de:08212:6:1:1", {neighbour.latitude, neighbour.longitude}},
        {7, "Tullastrasse", "de:08212:7:1:1", {49.006250, 8.431785}}};
    data.buildings = {{{49.006, 8.428}, {49.006, 8.429}, {49.005, 8.429}}};
    data.highways = {
        MappedHighway(20, "residential", {PixelPosition(140, 170), PixelPosition(650, 170)}),
        MappedHighway(21, "footway", {PixelPosition(190, 120), PixelPosition(190, 350)})};
    return data;
}

static const kvv::MapBackground& TestBackground() {
    static const kvv::MapBackground background(TestMapData());
    return background;
}

static kvv::LiveMap MakeFixtureMap(
    const kvv::TransitSnapshot& snapshot, const kvv::MapBackground& background) {
    return kvv::MakeLiveMap(snapshot, background, FixtureConfig());
}

// Exercise the application adapter against both public library contracts.
static kvv::LiveMap ParseMap(const std::string& text, int elapsed = 0,
    const kvv::MapBackground& background = TestBackground()) {
    return MakeFixtureMap(kvv::ParseDepartureResponse(text, elapsed), background);
}

static kvv::LiveMap EmptyMap() {
    kvv::LiveMap map;
    map.config = FixtureConfig();
    map.server_time = "2026-10-13T12:00:00";
    return map;
}

static void CheckLocalClock() {
    // These raster tests run before any worker threads start. Restore TZ even
    // when an assertion throws, so later parsing fixtures retain their zone.
    struct RestoreTimezone {
        const char* current = std::getenv("TZ");
        std::optional<std::string> previous =
            current != nullptr ? std::optional<std::string>(current) : std::nullopt;

        ~RestoreTimezone() {
            if (previous) {
                setenv("TZ", previous->c_str(), 1);
            } else {
                unsetenv("TZ");
            }
            tzset();
        }
    } restore;
    Require(setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1) == 0,
        "Could not configure the local-clock test timezone");
    tzset();

    const std::pair<const char*, const char*> times[] = {
        {"2026-01-14T12:34:56Z", "2026-01-14T13:34:56"},
        {"2026-07-14T22:30:00Z", "2026-07-15T00:30:00"},
        {"2025-12-31T23:30:00Z", "2026-01-01T00:30:00"},
        {"2026-03-29T00:59:59Z", "2026-03-29T01:59:59"},
        {"2026-03-29T01:00:00Z", "2026-03-29T03:00:00"},
        {"2026-10-25T01:00:00Z", "2026-10-25T02:00:00"}};
    for (const auto& [utc, local] : times) {
        auto actual = EmptyMap();
        actual.server_time = utc;
        auto expected = EmptyMap();
        expected.server_time = local;
        Require(Render(actual) == Render(expected),
            "UTC server time did not render with the local clock and date");
    }
}

static Frame RenderScreen(const std::optional<kvv::TransitSnapshot>& snapshot,
    const kvv::MapBackground* background, const kvv::MapScreenStatus& status = {},
    const kvv::MapConfig& config = FixtureConfig()) {
    Frame frame;
    MonochromeCanvas canvas(frame.data());
    kvv::DrawMapScreen(canvas, snapshot, background, status, config);
    return frame;
}

static void CheckLoadingScreen(const char* output_directory) {
    kvv::TransitSnapshot transit;
    transit.server_time = "2026-10-13T12:00:00";
    transit.stops = {Stop("live", 0, 0)};
    const kvv::MapBackground empty_background;
    const Frame pending = RenderScreen(std::nullopt, nullptr, {true, false, false});
    const Frame map_ready = RenderScreen(std::nullopt, &TestBackground());
    const Frame transit_ready = RenderScreen(transit, nullptr, {true, false, false});
    const Frame retrying = RenderScreen(std::nullopt, nullptr, {false, true, true});

    // Completion order changes progress only, without exposing a partial map.
    Require(pending != map_ready && pending != transit_ready && map_ready != transit_ready,
        "Loading screen does not distinguish which source is ready");
    Require(map_ready == RenderScreen(std::nullopt, &empty_background),
        "OSM geometry appeared before the first transit response");
    Require(transit_ready == RenderScreen(kvv::TransitSnapshot{}, nullptr, {true, false, false}),
        "Transit details appeared before the first map response");
    Require(retrying != pending, "Initial download failures do not show retry progress");
    Require(RenderScreen(std::nullopt, &TestBackground(), {false, false, true}) != map_ready &&
            RenderScreen(transit, nullptr, {false, true, false}) != transit_ready,
        "A failed missing source is not distinguished from an active download");

    // Neither the map header nor footer belongs on an incomplete screen.
    for (const Frame* loading : {&pending, &map_ready, &transit_ready, &retrying}) {
        bool visible_message = false;
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            for (int x = 0; x < EPD_WIDTH; ++x) {
                const bool black = Black(*loading, x, y);
                visible_message = visible_message || black;
                if (y < 82 || y >= 438) {
                    Require(!black, "Map header or footer leaked onto the loading screen");
                }
            }
        }
        Require(visible_message, "Loading screen is blank");
    }

    // Success with no departures or OSM features is still a complete response.
    const kvv::TransitSnapshot empty_transit;
    Require(RenderScreen(empty_transit, &empty_background) ==
            Render(MakeFixtureMap(empty_transit, empty_background)),
        "Valid empty responses leave the interface stuck on the loading screen");
    const Frame ready = RenderScreen(transit, &TestBackground());
    Require(ready == Render(MakeFixtureMap(transit, TestBackground())),
        "Complete data does not unlock the map interface");

    // A redraw must erase old geography and reset clipping from previous work.
    Frame reused = ready;
    MonochromeCanvas canvas(reused.data());
    canvas.SetClipRect(0, 0, 1, 1);
    kvv::DrawMapScreen(canvas, std::nullopt, nullptr, {true, false, false}, FixtureConfig());
    Require(reused == pending, "Loading redraw retained map pixels or inherited a clip");

    if (output_directory != nullptr) {
        Save(pending, std::string(output_directory) + "/map-loading.bmp");
        Save(map_ready, std::string(output_directory) + "/map-loading-departures.bmp");
        Save(retrying, std::string(output_directory) + "/map-loading-retrying.bmp");
    }
}

static void CheckLoadedScreenRefresh() {
    kvv::TransitSnapshot transit;
    transit.server_time = "2026-10-13T12:00:00";
    transit.stale = true;
    const auto map = MakeFixtureMap(transit, TestBackground());

    // Retained source snapshots keep the full interface available during retries.
    auto updating = map;
    updating.map_status = "UPDATING MAP";
    Require(RenderScreen(transit, &TestBackground(), {true, false, false}) == Render(updating),
        "Background refresh replaced a complete interface with the loading screen");
    auto cached = map;
    cached.map_status = "MAP CACHED - RETRYING";
    Require(RenderScreen(transit, &TestBackground(), {false, true, true}) == Render(cached) &&
            RenderScreen(transit, &TestBackground(), {true, true, true}) == Render(cached),
        "Refresh failure hid cached geometry or gave the wrong retry status");
    Require(RenderScreen(transit, &TestBackground(), {false, false, true}) == Render(map),
        "Transit refresh failure hid the previously complete interface");
}

static void CheckPlaceMarker() {
    const Frame blank = Render(EmptyMap());
    // After panning west/north, the configured place projects southeast to (415,281).
    // The permanent diamond remains visible even when no live vehicles exist.
    Require(Black(blank, 415, 281) && Black(blank, 425, 281) && !Black(blank, 420, 281),
        "Place diamond missing or incorrectly positioned");

    // The 228x24 label uses uniform 2x pixels, directly north of the diamond.
    Require(Black(blank, 301, 239) && Black(blank, 528, 262) && !Black(blank, 300, 239) &&
            !Black(blank, 529, 239),
        "Place label is not compact and directly north of its marker");

    Frame expected_text;
    MonochromeCanvas text_canvas(expected_text.data());
    text_canvas.Clear();
    text_canvas.DrawText(307, 244, "CUSTOM PLACE LABEL", 2);
    for (int y = 240; y < 262; ++y) {
        for (int x = 302; x < 528; ++x) {
            Require(Black(blank, x, y) == Black(expected_text, x, y),
                "Place lettering differs from the uniform integer-scale font");
        }
    }
}

// Station fixtures verify geographic placement independently of train estimates.
static void CheckStationMarkers() {
    auto map = EmptyMap();

    map.stops = {Stop("7000007", 0, 0)};
    const Frame station = Render(map);
    Require(Black(station, 554, 330) && !Black(station, 554, 337),
        "Viewport pan moved the geographic station or used the wrong direction");
    map.stops.clear();
    map.stops = {{"neighbour", "Neighbour", ViewPosition(-200, 0)},
        {"offscreen", "Offscreen", ViewPosition(2000, 0)}};
    const Frame neighbours = Render(map);
    Require(Black(neighbours, 246, 253) && !Black(neighbours, 246, 260),
        "Visible stations other than Tullastrasse were not rendered");
    map.stops.clear();
}

// Crossing segments must be clipped even when both endpoints are off-screen.
static void CheckTrackClipping() {
    auto map = EmptyMap();
    const Frame blank = Render(map);

    map.tracks = {{ViewPosition(-5000, 0), ViewPosition(5000, 0)},
        {ViewPosition(0, -5000), ViewPosition(0, 5000)},
        {PixelPosition(-5000, 150), PixelPosition(5000, 150)}};
    const Frame crossed = Render(map);
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        for (int x = 0; x < EPD_WIDTH; ++x) {
            if (y < 82 || y >= 438 || x < 16 || x >= 784) {
                Require(Black(blank, x, y) == Black(crossed, x, y), "Route escaped viewport");
            }
        }
    }
    Require(Black(crossed, 34, 260) && Black(crossed, 768, 260),
        "Map no longer uses the full display width");
    Require(Black(crossed, 400, 82) && Black(crossed, 400, 437), "Crossing segment was dropped");
    Require(Black(crossed, 16, 150) && Black(crossed, 783, 150),
        "Rails were clipped from the rectangular viewport corners");
}

static void CheckHighwayStyles() {
    auto map = EmptyMap();
    map.highways = {{{PixelPosition(140, 160), PixelPosition(280, 160)}, false},
        {{PixelPosition(200, 110), PixelPosition(200, 210)}, false},
        {{PixelPosition(140, 200), PixelPosition(180, 200)}, true}};
    const Frame streets = Render(map);

    // Hollow roads stay distinct from both solid rails and one-pixel buildings.
    Require(Black(streets, 160, 158) && Black(streets, 160, 162) && !Black(streets, 160, 159) &&
            !Black(streets, 160, 160) && !Black(streets, 160, 161) && !Black(streets, 160, 157),
        "Street casing is not a hollow five-pixel stroke");
    Require(!Black(streets, 198, 160) && !Black(streets, 200, 158) && !Black(streets, 200, 160) &&
            !Black(streets, 202, 160),
        "Street junction interiors are blocked by another road casing");
    std::reverse(map.highways.begin(), map.highways.end());
    Require(streets == Render(map), "Street junctions depend on OSM way order");

    int path_pixels = 0;
    for (int x = 140; x < 172; ++x) {
        path_pixels += Black(streets, x, 200) ? 1 : 0;
        Require(!Black(streets, x, 199) && !Black(streets, x, 201),
            "Footpath stroke is thicker than one pixel");
    }
    Require(path_pixels == 16, "Footpath does not alternate four-pixel dashes and gaps");

    // An OSM vertex inside a dash must not restart its phase.
    auto unsplit = EmptyMap();
    unsplit.highways = {{{PixelPosition(140, 200), PixelPosition(180, 200)}, true}};
    auto split = unsplit;
    split.highways.front().points.insert(
        split.highways.front().points.begin() + 1, PixelPosition(146, 200));
    Require(Render(split) == Render(unsplit), "Footpath dash phase restarts at an OSM vertex");
}

static void CheckHighwayClipping() {
    auto map = EmptyMap();
    const Frame blank = Render(map);
    map.highways = {{{PixelPosition(-5000, 160), PixelPosition(5000, 160)}, false},
        {{PixelPosition(770, -5000), PixelPosition(770, 5000)}, true}};
    const Frame crossed = Render(map);

    // Every way crosses the map with both endpoints outside the displayed area.
    Require(Black(crossed, 16, 158) && Black(crossed, 783, 162),
        "Street did not reach the rectangular viewport edges");
    bool path_visible = false;
    for (int y = 100; y < 120; ++y) {
        path_visible = path_visible || Black(crossed, 770, y);
    }
    Require(path_visible, "Footpath was clipped from the rectangular viewport corner");

    for (int y = 0; y < EPD_HEIGHT; ++y) {
        for (int x = 0; x < EPD_WIDTH; ++x) {
            if (y < 82 || y >= 438 || x < 16 || x >= 784) {
                Require(Black(blank, x, y) == Black(crossed, x, y),
                    "Street or footpath escaped the geographic clip");
            }
        }
    }
}

static void CheckHighwayLayerPriority() {
    auto map = EmptyMap();
    const Frame blank = Render(map);
    map.highways = {{{PixelPosition(130, 170), PixelPosition(280, 170)}, false},
        {{PixelPosition(130, 190), PixelPosition(280, 190)}, true},
        {{PixelPosition(350, 281), PixelPosition(480, 281)}, false},
        {{PixelPosition(350, 250), PixelPosition(480, 250)}, true}};
    map.tracks = {{PixelPosition(180, 140), PixelPosition(180, 210)}};
    map.buildings = {{PixelPosition(210, 140), PixelPosition(210, 210)}};
    map.stops = {{"platform", "Platform", PixelPosition(240, 170)}};
    const Frame layered = Render(map);

    Require(Black(layered, 178, 170) && Black(layered, 180, 170) && Black(layered, 182, 170),
        "Street interior erased the foreground rail");
    Require(Black(layered, 210, 170) && Black(layered, 210, 190),
        "Street or footpath erased a building outline");
    Require(Black(layered, 247, 170) && !Black(layered, 240, 168) && !Black(layered, 240, 170),
        "Street or footpath obscured the platform marker");
    Require(Black(layered, 415, 281) && Black(layered, 425, 281) && !Black(layered, 420, 281),
        "Street or footpath obscured the place marker");
    for (int y = 239; y <= 262; ++y) {
        for (int x = 301; x <= 528; ++x) {
            Require(Black(layered, x, y) == Black(blank, x, y),
                "Street or footpath obscured the place label");
        }
    }
}

static void CheckBuildingOutlines() {
    auto map = EmptyMap();
    const Frame blank = Render(map);

    // Buildings use outlines only, including separate courtyard rings.
    map.tracks.clear();
    map.buildings = {{ViewPosition(100, -50), ViewPosition(200, -50), ViewPosition(200, -150),
                         ViewPosition(100, -150), ViewPosition(100, -50)},
        {ViewPosition(125, -75), ViewPosition(175, -75), ViewPosition(175, -125),
            ViewPosition(125, -125), ViewPosition(125, -75)}};
    const Frame buildings = Render(map);
    Require(Black(buildings, 477, 337) && !Black(buildings, 478, 337),
        "Building outline missing or thicker than one pixel");
    Require(Black(buildings, 496, 337) && !Black(buildings, 516, 337),
        "Courtyard ring missing or building interior filled");
    map.buildings = {{ViewPosition(-5000, 0), ViewPosition(5000, 0)},
        {ViewPosition(0, -5000), ViewPosition(0, 5000)},
        {PixelPosition(-5000, 365), PixelPosition(5000, 365)}};
    const Frame clipped_buildings = Render(map);
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        for (int x = 0; x < EPD_WIDTH; ++x) {
            if (y < 82 || y >= 438 || x < 16 || x >= 784) {
                Require(Black(blank, x, y) == Black(clipped_buildings, x, y),
                    "Building escaped the map viewport");
            }
        }
    }
    Require(Black(clipped_buildings, 16, 365) && Black(clipped_buildings, 783, 365),
        "Buildings were clipped from the rectangular viewport corners");
    map.buildings.clear();
}

static void CheckCornerMarkers() {
    osm::MapData data{kvv::MapViewport(FixtureConfig()), {}, {}, {}};
    data.rails = {Track({PixelPosition(740, 365), PixelPosition(850, 365)}, {1, 2})};
    const auto platform = PixelPosition(775, 155);
    const auto east_outside = PixelPosition(790, 155);
    const auto south_outside = PixelPosition(100, 445);
    data.stops = {{99, "Corner platform", "", {platform.latitude, platform.longitude}},
        {100, "East outside", "", {east_outside.latitude, east_outside.longitude}},
        {101, "South outside", "", {south_outside.latitude, south_outside.longitude}}};

    kvv::TransitSnapshot transit;
    transit.stops = {{"corner-live", "Corner live platform", PixelPosition(25, 155)},
        {"west-outside", "West outside", PixelPosition(10, 155)},
        {"north-outside", "North outside", PixelPosition(100, 75)}};
    const auto train = PixelPosition(775, 365);
    const auto outside_train = PixelPosition(815, 365);
    transit.vehicles = {
        {"S2", "Corner", train, train, 0}, {"S9", "Outside", outside_train, outside_train, 0}};

    // Adapter filtering and raster clipping must agree on the same rectangle.
    const auto map = MakeFixtureMap(transit, kvv::MapBackground(data));
    Require(
        map.stops.size() == 2 && map.stops[0].id == "corner-live" && map.stops[1].id == "osm:99",
        "Visible corner platforms were dropped or offscreen platforms retained");
    Require(map.vehicles.size() == 1 && map.vehicles.front().line == "S2",
        "Visible corner train was dropped or offscreen train retained");

    const Frame rendered = Render(map);
    // Probe the platform rings near the left and right viewport edges.
    Require(Black(rendered, 25, 162) && Black(rendered, 775, 148),
        "Visible corner platforms did not render");
    Require(Black(rendered, 775, 361), "Visible corner train did not render");
}

static void CheckRailGeometry() {
    auto map = EmptyMap();

    // Equal metre distances must have equal screen lengths on both axes.
    // Keep scale probes clear of the configured place label.
    map.tracks = {{ViewPosition(-200, 100), ViewPosition(-100, 100)},
        {ViewPosition(-200, 100), ViewPosition(-200, 200)}};
    const Frame scale = Render(map);
    Require(Black(scale, 323, 183) && Black(scale, 246, 106), "Geography was stretched");

    // A curved rail must retain its bend, not draw a chord between platforms.
    map.tracks = {{ViewPosition(-100, 0), ViewPosition(0, 100), ViewPosition(100, 0)}};
    const auto curved = Render(map);
    Require(Black(curved, 400, 183) && !Black(curved, 400, 260), "Track bend became a chord");

    const kvv::RailGeometry rails({Track({Position(-100, 0), Position(0, 100)}, {1, 2}),
        Track({Position(0, 100), Position(100, 0)}, {2, 3})});
    const auto midpoint = rails.Interpolate(Position(-100, 0), Position(100, 0), 0.5);
    Require(midpoint && std::abs(midpoint->latitude - Position(0, 100).latitude) < 1e-7,
        "Train estimate cut across a curved rail");
    Require(!rails.Snap(Position(2000, 2000)), "Remote point snapped onto local rails");

    const kvv::RailGeometry disconnected({Track({Position(-200, 0), Position(-100, 0)}, {1, 2}),
        Track({Position(100, 0), Position(200, 0)}, {3, 4})});
    Require(!disconnected.Interpolate(Position(-150, 0), Position(150, 0), 0.5),
        "Invented a rail link between disconnected tracks");

    // Parsed gaps arrive as independent segments even when the way ID matches.
    auto first = Track({Position(-100, 0), Position(0, 0)}, {1, 2});
    auto second = Track({Position(200, 0), Position(300, 0)}, {4, 5});
    second.id = first.id;
    const kvv::RailGeometry gaps({first, second});
    Require(gaps.Tracks().size() == 2 && !gaps.Interpolate(Position(-50, 0), Position(250, 0), 0.5),
        "Cropped OSM geometry gap became a straight connection");
    Require(!kvv::RailGeometry{}.Snap(Position(0, 0)), "Empty map invented a rail match");
}

static void CheckEfaIntegration() {
    const auto map = EmptyMap();

    // EFA stop coordinates must never become rail lines.
    Json row = {{"stopID", "7000007"}, {"nameWO", "Tullastrasse"}, {"x", "8.431785"},
        {"y", "49.006250"},
        {"dateTime",
            {{"year", "2026"}, {"month", "10"}, {"day", "13"}, {"hour", "12"}, {"minute", "00"}}},
        {"servingLine", {{"motType", "4"}, {"symbol", "1"}, {"stateless", "test"}}},
        {"prevStopSeq",
            Json::array({Call("7000007", -70, 10, "20261013 11:56:00"),
                Call("outside", 0, 2000, "20261013 11:58:00")})}};
    const Json response = {
        {"parameters", Json::array({{{"name", "serverTime"}, {"value", map.server_time}}})},
        {"departureList", Json::array({row})}};
    const auto parsed = ParseMap(response.dump());
    Require(parsed.buildings.size() == TestBackground().buildings.size(),
        "Live refresh lost the static building background");
    Require(parsed.tracks.size() == TestBackground().rails.Tracks().size(),
        "Tracks depend on timetable stop sequences");
    Require(parsed.highways.size() == TestBackground().highways.size(),
        "Live refresh lost streets and footpaths");
    Require(std::count_if(parsed.stops.begin(), parsed.stops.end(),
                [](const auto& stop) {
                    return stop.id == "7000007";
                }) == 2,
        "Distinct station platform coordinates were collapsed or duplicated");
    Require(parsed.vehicles.size() == 1, "Timed local vehicle did not decode");
    Require(std::any_of(parsed.stops.begin(), parsed.stops.end(),
                [](const auto& stop) {
                    return stop.id == "7000006";
                }),
        "Visible station missing when it has no departure row");

    // Live platforms replace catalog entries without introducing duplicates.
    Json neighbouring_calls = response;
    neighbouring_calls["departureList"][0]["prevStopSeq"] =
        Json::array({Call("7000006", -500, 100, "20261013 11:58:00"),
            Call("7000006", -500, 100, "20261013 11:58:00"),
            Call("outside", 5000, 5000, "20261013 11:57:00")});
    const auto all_stops = ParseMap(neighbouring_calls.dump());
    Require(std::count_if(all_stops.stops.begin(), all_stops.stops.end(),
                [](const auto& stop) {
                    return stop.id == "7000006";
                }) == 1,
        "Neighbouring live station filtered out or duplicated by static map");
    Require(std::none_of(all_stops.stops.begin(), all_stops.stops.end(),
                [](const auto& stop) {
                    return stop.id == "outside";
                }),
        "Offscreen station included in visible station list");

    auto turn_data = TestMapData();
    turn_data.rails = {Track({Position(0, 0), Position(100, 0), Position(100, 100)}, {1, 2, 3})};
    const kvv::MapBackground turn(turn_data);
    Json moving = response;
    moving["departureList"][0]["prevStopSeq"] = Json::array();
    moving["departureList"][0]["onwardStopSeq"] =
        Json::array({Call("next", 100, 100, "20261013 12:02:00")});
    const auto moved = ParseMap(moving.dump(), 60, turn);
    Require(moved.vehicles.size() == 1 &&
            std::abs(moved.vehicles[0].position.latitude - Position(100, 0).latitude) < 1e-5 &&
            std::abs(moved.vehicles[0].position.longitude - Position(100, 0).longitude) < 1e-5,
        "Per-second motion did not follow the rail bend after departure");
}

static void CheckMapDataIntegration() {
    const auto bounds = kvv::MapViewport(FixtureConfig());
    const auto southwest = ViewPosition(-500, -356.0 / (2 * 0.768));
    const auto northeast = ViewPosition(500, 356.0 / (2 * 0.768));
    Require(std::abs(bounds.south - southwest.latitude) < 1e-10 &&
            std::abs(bounds.west - southwest.longitude) < 1e-10 &&
            std::abs(bounds.north - northeast.latitude) < 1e-10 &&
            std::abs(bounds.east - northeast.longitude) < 1e-10,
        "OSM query bounds no longer match the displayed viewport");

    kvv::TransitSnapshot transit;
    transit.stale = true;
    const auto empty = MakeFixtureMap(transit, kvv::MapBackground{});
    Require(empty.buildings.empty() && empty.tracks.empty() && empty.highways.empty(),
        "Map still uses bundled geometry");
    const auto first = MakeFixtureMap(transit, TestBackground());
    Require(!first.buildings.empty() && !first.tracks.empty() && first.stops.size() == 2 &&
            first.highways.size() == 2,
        "OSM layers did not reach the renderer");

    auto replacement = TestMapData();
    replacement.buildings.clear();
    replacement.highways.clear();
    replacement.rails.front().service = "siding";
    replacement.stops.front().ifopt = "de:99999:6:1:1";
    const auto changed = MakeFixtureMap(transit, kvv::MapBackground(replacement));
    Require(changed.buildings.empty() && changed.highways.empty() &&
            changed.track_stroke_widths.front() == 1 && changed.stops.front().id == "osm:6",
        "New OSM geometry or provider mapping ignored");
    auto loading = empty;
    loading.map_status = "LOADING MAP";
    Require(Render(loading) != Render(empty), "OSM loading status is invisible");
}

static void CheckHighwayMapDataIntegration() {
    auto data = TestMapData();
    data.highways = {
        MappedHighway(30, "residential", {PixelPosition(140, 160), PixelPosition(280, 160)})};
    const std::vector<std::string> path_kinds = {
        "footway", "path", "steps", "pedestrian", "cycleway", "bridleway"};
    for (const auto& kind : path_kinds) {
        data.highways.push_back(MappedHighway(
            31 + data.highways.size(), kind, {PixelPosition(140, 200), PixelPosition(180, 200)}));
    }
    const kvv::MapBackground background(data);
    const auto map = MakeFixtureMap(kvv::TransitSnapshot{}, background);
    Require(map.highways.size() == data.highways.size() && !map.highways.front().footpath,
        "Typed OSM streets did not reach the renderer");
    for (std::size_t index = 0; index < map.highways.size(); ++index) {
        const auto& way = map.highways[index];
        Require(way.footpath == (index > 0), "OSM highway type mapped to the wrong style");
        Require(way.points.size() == data.highways[index].points.size() &&
                way.points.front().latitude == data.highways[index].points.front().latitude &&
                way.points.front().longitude == data.highways[index].points.front().longitude,
            "OSM highway coordinates changed in the application adapter");
    }

    const Frame integrated = Render(map);
    Require(Black(integrated, 160, 158) && !Black(integrated, 160, 160),
        "Queried street did not render with a hollow casing");
    int path_pixels = 0;
    for (int x = 140; x < 172; ++x) {
        path_pixels += Black(integrated, x, 200) ? 1 : 0;
    }
    Require(path_pixels == 16, "Queried footpath did not render with dashes");
}

static void CheckMapConfiguration() {
    const kvv::MapConfig defaults;
    Require(defaults.places.empty(), "Default map unexpectedly adds a fixed place marker");
    Require(std::abs(defaults.center.latitude - 48.994369) < 1e-9 &&
            std::abs(defaults.center.longitude - 8.399670) < 1e-9,
        "Default map is not centered on Hauptbahnhof Vorplatz");
    const auto default_bounds = kvv::MapViewport();
    Require(std::abs((default_bounds.south + default_bounds.north) / 2 - defaults.center.latitude) <
                1e-10 &&
            std::abs((default_bounds.west + default_bounds.east) / 2 - defaults.center.longitude) <
                1e-10,
        "Default query bounds do not use the configured center");
    auto blank = EmptyMap();
    blank.config = defaults;
    const Frame default_frame = Render(blank);
    for (int y = 82; y < 438; ++y) {
        for (int x = 16; x < 784; ++x) {
            Require(!Black(default_frame, x, y), "Unconfigured map still renders a fixed place");
        }
    }

    kvv::MapConfig relocated = FixtureConfig();
    relocated.center = {60.0, 10.0};
    relocated.places.clear();
    const auto bounds = kvv::MapViewport(relocated);
    const auto original = kvv::MapViewport(FixtureConfig());
    Require(std::abs((bounds.south + bounds.north) / 2 - relocated.center.latitude) < 1e-10 &&
            std::abs((bounds.west + bounds.east) / 2 - relocated.center.longitude) < 1e-10,
        "Configured map center did not move the OSM query bounds");
    Require(std::abs((bounds.north - bounds.south) - (original.north - original.south)) < 1e-10 &&
            std::abs((bounds.east - bounds.west) * std::cos(60.0 * 3.141592653589793 / 180.0) -
                (original.east - original.west) *
                    std::cos(FixtureConfig().center.latitude * 3.141592653589793 / 180.0)) < 1e-10,
        "Relocated viewport changed its size in metres or ignored longitude scaling");

    kvv::TransitSnapshot snapshot;
    snapshot.stops = {{"relocated", "RELOCATED STATION", relocated.center}};
    const kvv::MapBackground background;
    const auto map = kvv::MakeLiveMap(snapshot, background, relocated);
    Require(map.stops.size() == 1 && map.nearest_station &&
            map.nearest_station->id == "relocated" &&
            map.config.center.latitude == relocated.center.latitude &&
            map.config.center.longitude == relocated.center.longitude,
        "Configured center did not reach stop filtering or nearest-station selection");
    Require(MakeFixtureMap(snapshot, background).stops.empty(),
        "Relocated stop leaked into the unrelated fixture viewport");
    const Frame centered = Render(map);
    Require(Black(centered, 400, 253) && !Black(centered, 400, 260),
        "Configured center is not the center of the rendered map");
    Require(RenderScreen(snapshot, &background, {}, relocated) == centered,
        "DrawMapScreen ignored the supplied map configuration");
}

static void CheckCustomPlaces() {
    auto map = EmptyMap();
    map.config.places.clear();
    const Frame blank = Render(map);
    map.config.places = {
        {PixelPosition(200, 220), "WEST PLACE"}, {PixelPosition(600, 330), "EAST PLACE"}};
    const Frame places = Render(map);
    Require(Black(places, 200, 220) && Black(places, 210, 220) && !Black(places, 205, 220) &&
            Black(places, 600, 330) && Black(places, 610, 330) && !Black(places, 605, 330),
        "Multiple configured place markers were missing or misplaced");
    map.config.places.push_back({PixelPosition(5000, 260), "OFFSCREEN PLACE"});
    Require(Render(map) == places, "Offscreen place produced a marker or label inside the map");

    map.config.places.push_back({PixelPosition(775, 95), std::string(200, 'W')});
    const Frame long_label = Render(map);
    Require(Black(long_label, 775, 95), "Long place label hid its geographic marker");
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        for (int x = 0; x < EPD_WIDTH; ++x) {
            if (y < 82 || y >= 438 || x < 16 || x >= 784) {
                Require(Black(long_label, x, y) == Black(blank, x, y),
                    "Configured place or long label escaped the map viewport");
            }
        }
    }
}

static void CheckNearestStation() {
    const kvv::GeoPoint center{60.0, 10.0};
    Require(!kvv::NearestStation({}, center), "Empty station catalog invented a nearest station");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    std::vector<kvv::Stop> stations = {{"blank", "", center}, {"whitespace", " \t\n", center},
        {"nan", "INVALID", {nan, 10}}, {"infinite", "INVALID", {60, infinity}},
        {"latitude", "INVALID", {91, 10}}, {"longitude", "INVALID", {60, 181}}};
    Require(!kvv::NearestStation(stations, center), "Invalid or unnamed station was selected");
    stations.push_back({"north", "NORTH", {60.004, 10.0}});
    stations.push_back({"east", "EAST", {60.0, 10.006}});
    const auto nearest = kvv::NearestStation(stations, center);
    Require(nearest && nearest->id == "east",
        "Nearest station used degree differences instead of geographic distance");
    const auto moved = kvv::NearestStation(stations, {60.004, 10.0});
    Require(moved && moved->id == "north", "Nearest station did not follow the configured center");

    kvv::MapBackground background;
    background.stops = {{"catalog-nearest", "NORTH CATALOG", ViewPosition(0, 300)},
        {"catalog-visible", "EAST CATALOG", ViewPosition(400, 0)}};
    kvv::TransitSnapshot snapshot;
    const auto catalog_map = MakeFixtureMap(snapshot, background);
    Require(catalog_map.vehicles.empty() && catalog_map.stops.size() == 1 &&
            catalog_map.nearest_station && catalog_map.nearest_station->id == "catalog-nearest",
        "Nearest station depended on trains or discarded the offscreen station catalog");
    snapshot.stops = {{"feed-nearest", "FEED STATION", ViewPosition(100, 0)}};
    const auto feed_map = MakeFixtureMap(snapshot, background);
    Require(feed_map.nearest_station && feed_map.nearest_station->id == "feed-nearest",
        "Live station catalog did not participate in nearest-station selection");

    // One provider station can have platforms on both sides of the viewport.
    // An offscreen live platform must not suppress its nearer catalog platform.
    background.stops = {{"station-a", "CATALOG A", ViewPosition(40, 0)},
        {"station-b", "CATALOG B", ViewPosition(100, 0)}};
    snapshot.stops = {{"station-a", "LIVE A", ViewPosition(800, 0)}};
    const auto platforms = MakeFixtureMap(snapshot, background);
    Require(platforms.nearest_station && platforms.nearest_station->name == "CATALOG A" &&
            platforms.stops.size() == 2 &&
            std::any_of(platforms.stops.begin(), platforms.stops.end(),
                [](const auto& stop) {
                    return stop.id == "station-a" && stop.name == "CATALOG A";
                }),
        "Offscreen live platform hid the nearest catalog platform or changed the header");

    auto renamed = catalog_map;
    renamed.nearest_station->name = "DIFFERENT STATION";
    const Frame first_header = Render(catalog_map);
    const Frame second_header = Render(renamed);
    bool header_changed = false;
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        for (int x = 0; x < EPD_WIDTH; ++x) {
            if (y < 70) {
                header_changed =
                    header_changed || Black(first_header, x, y) != Black(second_header, x, y);
            } else {
                Require(Black(first_header, x, y) == Black(second_header, x, y),
                    "Nearest-station title changed unrelated map content");
            }
        }
    }
    Require(header_changed, "Map header ignored the nearest station's name");

    Frame old_title;
    MonochromeCanvas canvas(old_title.data());
    canvas.Clear();
    canvas.DrawText(20, 14, "TULLASTRASSE", 3);
    const Frame no_station = Render(EmptyMap());
    bool fixed_title = true;
    for (int y = 14; y < 35; ++y) {
        for (int x = 20; x < 20 + MonochromeCanvas::TextWidth("TULLASTRASSE", 3); ++x) {
            fixed_title = fixed_title && Black(no_station, x, y) == Black(old_title, x, y);
        }
    }
    Require(!fixed_title, "Map without a nearest station retained the fixed Tullastrasse title");
}

static void CheckStationTitle() {
    auto map = EmptyMap();
    map.nearest_station = kvv::Stop{"title", "SHORT TITLE", map.config.center};
    const Frame short_title = Render(map);
    map.nearest_station->name = "Äußere Straße (Östlicher Übergang) " + std::string(160, 'W');
    const Frame german_title = Render(map);
    map.nearest_station->name = "AEUSSERE STRASSE OESTLICHER UEBERGANG " + std::string(160, 'W');
    Require(german_title == Render(map), "German station title lost readable umlauts or sharp S");

    const int clock_left = 780 - MonochromeCanvas::TextWidth("12:00:00", 3);
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        for (int x = 0; x < EPD_WIDTH; ++x) {
            if (x >= clock_left - 10 || x < 20 || y >= 70) {
                Require(Black(german_title, x, y) == Black(short_title, x, y),
                    "Long station title covered the clock, date, or map");
            }
        }
    }
}

static void CheckHighLatitudeRails() {
    const auto polar = [](double east, double north) {
        const double longitude_scale = 111320.0 * std::cos(70.0 * 3.141592653589793 / 180.0);
        return kvv::GeoPoint{70.0 + north / 111320.0, 10.0 + east / longitude_scale};
    };
    const kvv::RailGeometry parallel({Track({polar(0, -100), polar(0, 100)}, {1, 2}),
        Track({polar(200, -100), polar(200, 100)}, {3, 4})});
    const auto snapped = parallel.Snap(polar(60, 0));
    Require(snapped && std::abs(snapped->longitude - polar(0, 0).longitude) < 1e-7,
        "Rail snapping used the original city's longitude scale at high latitude");
    const kvv::RailGeometry bend({Track({polar(0, 0), polar(100, 0), polar(100, 100)}, {1, 2, 3})});
    const auto midpoint = bend.Interpolate(polar(0, 0), polar(100, 100), 0.5);
    Require(midpoint && std::abs(midpoint->latitude - polar(100, 0).latitude) < 1e-7 &&
            std::abs(midpoint->longitude - polar(100, 0).longitude) < 1e-7,
        "Equal metre rail segments no longer meet at their temporal midpoint");
}

static void CheckCrowdedMap(const char* output_directory) {
    auto map = EmptyMap();
    const Frame blank = Render(map);

    // Previews reproduce a junction with overlapping trains and boundary cases.
    map.tracks = TestBackground().rails.Tracks();
    map.buildings = TestBackground().buildings;
    map.highways = TestBackground().highways;
    map.track_stroke_widths = TestBackground().rails.StrokeWidths();
    map.stops = {Stop("7000007", 0, 0), Stop("7000007", 28, -12), Stop("7000007", -30, 90),
        Stop("7000007", -18, 120)};

    if (output_directory != nullptr) {
        Save(Render(map), std::string(output_directory) + "/map-empty.bmp");
    }
    map.vehicles = {{"S2", "Test", Position(-38, 70)}, {"S51", "Test", Position(40, -12)}};
    if (output_directory != nullptr) {
        Save(Render(map), std::string(output_directory) + "/map-two-trains.bmp");
    }

    // Dense overlapping vehicles must leave the configured place readable.
    map.vehicles.clear();
    for (int index = 0; index < 8; ++index) {
        map.vehicles.push_back({"S" + std::to_string(index + 1), "Test", Position(index * 2, 0)});
    }
    map.vehicles.push_back({"S51", "Test", ViewPosition(496, 0)});
    map.vehicles.push_back({"17", "Test", ViewPosition(0, 229)});
    const Frame crowded = Render(map);
    Require(Black(crowded, 415, 281) && Black(crowded, 425, 281) && !Black(crowded, 420, 281),
        "Trains obscured the place marker");
    for (int y = 239; y <= 262; ++y) {
        for (int x = 301; x <= 528; ++x) {
            Require(Black(crowded, x, y) == Black(blank, x, y),
                "Place label moved or was obscured by the busy map");
        }
    }
    if (output_directory != nullptr) {
        Save(crowded, std::string(output_directory) + "/map-crowded.bmp");
    }

    // Provider row order must not move the chosen labels.
    std::reverse(map.vehicles.begin(), map.vehicles.end());
    Require(crowded == Render(map), "Labels moved when the response order changed");
}

// Optional manual preview uses a saved provider response, never a live request.
static void ExportCapturedMap(const char* response_file, const char* output_directory) {
    std::ifstream file(response_file);
    Require(file.good(), "Could not open captured EFA response");
    std::ostringstream text;
    text << file.rdbuf();
    const auto captured = ParseMap(text.str());
    Save(Render(captured), std::string(output_directory) + "/map-capture.bmp");
    std::cerr << "Captured response: " << captured.tracks.size() << " track ways, "
              << captured.stops.size() << " platforms, " << captured.vehicles.size()
              << " vehicles\n";
}

int main(int argc, char** argv) {
    try {
        CheckLocalClock();
        CheckLoadingScreen(argc > 1 ? argv[1] : nullptr);
        CheckLoadedScreenRefresh();
        CheckPlaceMarker();
        CheckMapConfiguration();
        CheckCustomPlaces();
        CheckNearestStation();
        CheckStationTitle();
        CheckStationMarkers();
        CheckTrackClipping();
        CheckHighwayStyles();
        CheckHighwayClipping();
        CheckHighwayLayerPriority();
        CheckBuildingOutlines();
        CheckCornerMarkers();
        CheckRailGeometry();
        CheckHighLatitudeRails();
        CheckEfaIntegration();
        CheckMapDataIntegration();
        CheckHighwayMapDataIntegration();
        CheckCrowdedMap(argc > 1 ? argv[1] : nullptr);

        if (argc > 2) {
            ExportCapturedMap(argv[2], argv[1]);
        }

        std::cout << "Map rendering, geometry and EFA integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
