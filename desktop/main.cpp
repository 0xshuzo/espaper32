#include <epd7in5_V2.h>
#include <epd_simulator.h>
#include <transit_refresh.h>
#include <osm_map_refresh.h>
#include "kvv_live_map.h"
#include "monochrome_canvas.h"
#include <cstdlib>
#include <iostream>

int main() {
    unsigned char frame_buffer[EPD_FRAME_BYTES];
    Epd epd;
    MonochromeCanvas canvas(frame_buffer);
    const kvv::MapConfig config;

    // A saved response freezes estimates while exercising the same UI.
    const char* fixture = std::getenv("KVV_EFA_RESPONSE_FILE");
    const bool replay = fixture && *fixture;
    std::optional<kvv::Stop> departure_station;
    // Resolve the nearest supported station once, then refresh that board.
    // Both network operations run inside TransitRefresh's background worker.
    kvv::TransitRefresh refresh(
        [center = config.center, replay, &departure_station] {
            if (!replay && !departure_station) {
                departure_station = kvv::FindNearestStop(center);
            }
            return kvv::DownloadDepartureResponse(departure_station ? departure_station->id : "");
        },
        replay);
    osm::MapRefresh map_refresh;
    const auto viewport = kvv::MapViewport(config);
    std::shared_ptr<const osm::MapData> map_data;
    kvv::MapBackground background;
    std::string last_map_error;

    using Clock = kvv::TransitRefresh::Clock;
    auto next_draw = Clock::time_point{};

    // Poll frequently, but paint only on fresh data or the one-second cadence.
    return epd_simulator::Run(epd, "7.5-inch e-Paper", [&] {
        const auto now = Clock::now();
        const bool updated = refresh.Poll(now);
        const bool map_updated = map_refresh.Poll(viewport, now);
        if (const auto data = map_refresh.Snapshot(viewport); data != map_data) {
            // Rebuild the routing graph only when a new OSM result arrives.
            map_data = data;
            background = data ? kvv::MapBackground(*data) : kvv::MapBackground{};
        }
        if (last_map_error != map_refresh.Error()) {
            last_map_error = map_refresh.Error();
            if (!last_map_error.empty()) {
                std::cerr << "OSM: " << last_map_error << '\n';
            }
        }

        if (updated || map_updated || now >= next_draw) {
            // Reveal the interface only after both data sources are ready.
            kvv::DrawMapScreen(canvas, refresh.Snapshot(now), map_data ? &background : nullptr,
                {map_refresh.Loading(), !map_refresh.Error().empty(), !refresh.Error().empty()},
                config);

            epd.DisplayFrame(frame_buffer);
            next_draw = now + std::chrono::seconds(1);
        }
    });
}
