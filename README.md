# espaper32

ESP32 display application and its desktop integration. Owns the firmware entry
point, monochrome canvas, map rendering, train-to-rail matching and application
integration. Display implementations, EFA processing and OSM querying/parsing
are provided by separate libraries.

The current firmware draws a hardware status screen. The desktop application
shows the live map. Wi-Fi fetching and a memory-efficient ESP32 data path are
still future work.

## Repository boundaries

| Repository | Owns | Dependencies on these repos |
| --- | --- | --- |
| [epd7in5_v2_driver](../epd7in5_v2_driver) | Shared `Epd` API and physical SPI driver | None |
| [epd7in5_v2_simulator](../epd7in5_v2_simulator) | SDL display implementation and event loop | Driver API |
| [kvv_efa_query](../kvv_efa_query) | EFA query, parsing, timing, retention and polling | None |
| [osm_query_parser](../osm_query_parser) | OSM query, parsing and viewport cache | None |
| espaper32 | Firmware, UI, rail matching and application integration | Driver on ESP32; simulator, EFA and OSM on desktop |

`MapViewport()` derives geographic bounds from the rendering projection.
`osm::MapRefresh` queries the corresponding area in the background.
`MapBackground` prepares its typed buildings, streets, paths, stops and rail
graph once per new OSM result. `MakeLiveMap` combines this background with
current EFA estimates. Highway classification and map styling belong here;
the OSM library supplies the geographic geometry and original highway tags.
No OSM JSON parsing or embedded map data remains in this application.

## Desktop build

Checkouts can have arbitrary directory names and nesting. CMake identifies source
providers through their `cmake/capabilities.txt` declarations, then verifies the
required targets. It searches this source tree and any directories listed in
`SOURCE_DEPENDENCY_ROOTS` (a semicolon-separated CMake setting or environment
variable). Missing and ambiguous matches report the component and the setting
needed to locate it.

On Ubuntu/WSL, install a C++17 compiler, CMake, SDL2 development files, libcurl
7.85+ development files (`libcurl4-openssl-dev`), curl, git and Python 3 for tests.
JSON can come from an installed nlohmann/json CMake package or a source checkout
found by its contents. Initialize bundled submodules with
`git submodule update --init --recursive` in each checkout that uses them.
Dependencies are not downloaded during configuration.

```bash
cmake -S . -B build -DSOURCE_DEPENDENCY_ROOTS="/path/to/libraries;/another/source-tree"
cmake --build build -j
./build/epaper_simulator
ctest --test-dir build --output-on-failure
./build/map_render_checks build
```

To select a specific provider, set its source directory explicitly:

| CMake setting | Required component |
| --- | --- |
| `DISPLAY_API_SOURCE_DIR` | Display interface |
| `DISPLAY_SIMULATOR_SOURCE_DIR` | Desktop display simulator |
| `TRANSIT_QUERY_SOURCE_DIR` | Transit query library |
| `MAP_QUERY_SOURCE_DIR` | Map query library |

Existing targets supplied by a parent CMake project are reused. Source matches
are deduplicated by their real paths; multiple providers require an explicit
selection. Generated build trees and hidden directories are excluded from scans.
Use `JSON_SOURCE_DIR` to select a JSON source checkout, or `SDL2_DIR` and
`CMAKE_PREFIX_PATH` for installed dependencies outside the standard package locations.

`build/map-*.bmp` files are deterministic layout previews using synthetic map
geometry. Library and simulator tests run in their respective repositories.

For fully offline replay, supply both responses:

```bash
KVV_EFA_RESPONSE_FILE=/path/to/efa.json \
OSM_RESPONSE_FILE=/path/to/osm.json ./build/epaper_simulator
```

EFA input is polled every five seconds and redrawn every second. Prediction
updates preserve each train's position and adjust its speed to catch up or slow
down without moving backward. Motion follows the rail route through stops and
freezes after 30 seconds without fresh data. Replay freezes estimates at its
timestamp. The header shows provider time and its date in `DD.MM.YYYY` format.
OSM data is cached for one hour and saved across restarts. A saved map is shown
immediately, even if it is expired or the provider is unavailable, while refreshes
run in the background. Failed requests back off; provider bandwidth/rate limits
use their own cooldown. The large, unfiltered OSM editing-API fallback is opt-in.
The query tries up to 500 m of extra context for adjoining rail paths. If the
response exceeds 32 MiB, the background loader adjusts that margin to find the
largest fitting area, keeping the visible map intact. It publishes usable data
as soon as it arrives and refines the margin within 10 m, with at most eight
query attempts per search. A full-screen loading view reports map and departure
progress until both sources have returned their first valid response. Empty
results count as loaded; initial failures show retry status and keep the loading
view visible. Once loaded, the interface keeps cached geometry and the last transit snapshot visible during refreshes or outages.
See the [OSM README](../osm_query_parser/README.md)
for endpoints, cache behavior and replay requirements.

Edit [`src/map/map_config.h`](src/map/map_config.h) and rebuild to choose the
map's `center` (WGS84 latitude, longitude in decimal degrees) and optional
`places` entries (`{{latitude, longitude}, "MY PLACE"}`). The default center is
Karlsruhe Hauptbahnhof (Vorplatz), **48.994369, 8.399670**, with no custom markers.
The desktop resolves the nearest supported EFA station within 1 km of these
coordinates once at startup, then refreshes its departure board. Transit data
still uses the KVV provider and its rail/tram coverage. Offline replay skips this
lookup; choose captures covering the configured location.

The top-left header names the geographically closest named station in the
available OSM/EFA catalog, including stations without departures. If the catalog
has no usable stations, it displays `NO NEARBY STATION`. For drawing platforms,
visible live coordinates take precedence over matching OSM entries. Long station
names wrap; the display's ASCII font spells German umlauts as AE/OE/UE and ß as SS.

The map fills its 768 × 356 pixel rectangle, covering 1,000 m horizontally.
Geographic proportions are preserved, and features in all four corners are
included. It draws streets, footpaths, buildings, passenger rails, thinner
sidings, station platforms and any configured place markers. Place labels are
reserved before train labels and clipped to the map. Streets use hollow outlines;
footways, paths, steps, pedestrian ways, cycleways and bridleways use thin dashes.
Road junctions remain open, and path dashes continue across OSM vertices. All geographic strokes are
clipped to the map area. Streets and paths sit behind building outlines and
rails, while platform markers and labels remain in the foreground.
Karlsruhe IFOPT IDs (`de:08212:<stop>:...`) are associated with EFA IDs
`7000000 + <stop>` by the application. Other OSM stops keep their OSM identity.
Train labels follow estimates along connected rails; unmatched/disconnected
paths have no straight-line fallback. These are timetable estimates, not GPS.

## Map attribution

Live map data is © OpenStreetMap contributors, licensed under the
[Open Database License](https://opendatacommons.org/licenses/odbl/1-0/).

## ESP32 build

Install PlatformIO and keep the driver repo alongside this one, then run:

```bash
pio run -e esp32-hardware
```

`platformio.ini` references the driver via `symlink://../epd7in5_v2_driver` and
compiles `src/main.cpp` plus `src/monochrome_canvas.cpp`. Desktop integration,
OSM and EFA transports are excluded from firmware builds. The default board
is `esp32dev`; adjust `EPD_*_PIN` flags for your wiring.
[Driver documentation](../epd7in5_v2_driver/README.md) describes pins and the
48,000-byte framebuffer format.
