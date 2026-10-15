#include "kvv_live_map.h"
#include "monochrome_canvas.h"
#include <charconv>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <limits>
#include <optional>
#include <set>

namespace kvv {
namespace {

// Use the entire display width at a uniform metres-to-pixels scale. The wider
// screen crops the north/south extent; it does not stretch the geography.
constexpr double kEastHalfSpan = 500.0;
constexpr int kPlaceLabelScale = 2;
constexpr double kMetresPerLatitudeDegree = 111320.0;
constexpr double kRadiansPerDegree = 3.141592653589793 / 180.0;

constexpr int kMapLeft = 16;
constexpr int kMapTop = 82;
constexpr int kMapWidth = 768;
constexpr int kMapHeight = 356;
constexpr int kMapCenterX = kMapLeft + kMapWidth / 2;
constexpr int kMapCenterY = kMapTop + kMapHeight / 2;
constexpr double kPixelsPerMetre = kMapWidth / (2.0 * kEastHalfSpan);
constexpr double kNorthHalfSpan = kMapHeight / (2.0 * kPixelsPerMetre);

struct LocalPoint {
    double x;
    double y;
};

// Clip each original track segment, even when both endpoints are outside.
bool ClipSegment(LocalPoint& from, LocalPoint& to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    double enter = 0.0;
    double leave = 1.0;

    // Intersect the segment with each edge of the rectangular viewport.
    const auto clip = [&](double p, double q) {
        if (std::abs(p) < 1e-12) {
            return q >= 0.0;
        }
        const double t = q / p;
        if (p < 0.0) {
            enter = std::max(enter, t);
        } else {
            leave = std::min(leave, t);
        }
        return enter <= leave;
    };

    if (!clip(-dx, from.x + kEastHalfSpan) || !clip(dx, kEastHalfSpan - from.x) ||
        !clip(-dy, from.y + kNorthHalfSpan) || !clip(dy, kNorthHalfSpan - from.y)) {
        return false;
    }

    to = {from.x + leave * dx, from.y + leave * dy};
    from = {from.x + enter * dx, from.y + enter * dy};
    return true;
}

struct ScreenPoint {
    int x = 0;
    int y = 0;
};

ScreenPoint ProjectLocal(LocalPoint local) {
    return {static_cast<int>(std::lround(kMapCenterX + local.x * kPixelsPerMetre)),
        static_cast<int>(std::lround(kMapCenterY - local.y * kPixelsPerMetre))};
}

class MapProjection {
public:
    explicit MapProjection(GeoPoint center)
        : center_(center), longitude_scale_(kMetresPerLatitudeDegree *
                               std::cos(center.latitude * kRadiansPerDegree)) {
    }

    LocalPoint Localise(GeoPoint point) const {
        return {(point.longitude - center_.longitude) * longitude_scale_,
            (point.latitude - center_.latitude) * kMetresPerLatitudeDegree};
    }

    bool InMap(GeoPoint point) const {
        const auto local = Localise(point);
        return std::abs(local.x) <= kEastHalfSpan && std::abs(local.y) <= kNorthHalfSpan;
    }

    ScreenPoint Project(GeoPoint point) const {
        return ProjectLocal(Localise(point));
    }

    osm::BoundingBox Bounds() const {
        return {center_.latitude - kNorthHalfSpan / kMetresPerLatitudeDegree,
            center_.longitude - kEastHalfSpan / longitude_scale_,
            center_.latitude + kNorthHalfSpan / kMetresPerLatitudeDegree,
            center_.longitude + kEastHalfSpan / longitude_scale_};
    }

private:
    GeoPoint center_;
    double longitude_scale_;
};

// The display font is ASCII; retain readable German names from provider data.
std::string DisplayLabel(std::string text) {
    for (const auto& [from, to] : {std::pair{"ä", "AE"}, {"ö", "OE"}, {"ü", "UE"}, {"Ä", "AE"},
             {"Ö", "OE"}, {"Ü", "UE"}, {"ß", "SS"}, {"ẞ", "SS"}}) {
        std::size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos) {
            text.replace(position, std::char_traits<char>::length(from), to);
            position += std::char_traits<char>::length(to);
        }
    }
    std::string normalized;
    for (char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)) || character == '(' ||
            character == ')') {
            if (!normalized.empty() && normalized.back() != ' ') {
                normalized += ' ';
            }
        } else {
            normalized += character;
        }
    }
    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

std::string FitLabel(std::string text, int width, int scale) {
    const auto limit = static_cast<std::size_t>(width / MonochromeCanvas::TextWidth("M", scale));
    if (text.size() > limit) {
        text = text.substr(0, limit > 3 ? limit - 3 : 0) + "...";
    }
    return text;
}

std::string LocalServerTime(const std::string& server_time) {
    // Legacy captures already contain local civil time. The current API labels
    // UTC explicitly so both the clock and date need the desktop's local zone.
    if (server_time.empty() || server_time.back() != 'Z') {
        return server_time;
    }

    std::tm utc{};
    const char* end = strptime(server_time.c_str(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    if (end == nullptr || *end != '\0') {
        return {};
    }
    const std::time_t seconds = timegm(&utc);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return {};
    }

    char text[20];
    if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local) == 0) {
        return {};
    }
    return text;
}

std::string DisplayTime(const std::string& server_time) {
    return server_time.size() >= 19 ? server_time.substr(11, 8) : "--:--:--";
}

std::string DisplayDate(const std::string& server_time) {
    // Use the clock's local date, displayed in German date order.
    if (server_time.size() < 10) {
        return "--.--.----";
    }
    return server_time.substr(8, 2) + "." + server_time.substr(5, 2) + "." +
        server_time.substr(0, 4);
}

std::string VehicleLabel(const std::string& line) {
    return line.empty() ? "?" : line.substr(0, 4);
}

struct LabelBox {
    int x, y, width, height;
};

bool Overlaps(const LabelBox& a, const LabelBox& b) {
    constexpr int gap = 6;
    return a.x < b.x + b.width + gap && a.x + a.width + gap > b.x && a.y < b.y + b.height + gap &&
        a.y + a.height + gap > b.y;
}

std::optional<LabelBox> PlaceLabel(
    ScreenPoint point, int width, const std::vector<LabelBox>& occupied, int height = 33) {
    const auto fits = [&](const LabelBox& box) {
        return box.x >= kMapLeft + 3 && box.y >= kMapTop + 3 &&
            box.x + box.width <= kMapLeft + kMapWidth - 3 &&
            box.y + box.height <= kMapTop + kMapHeight - 3 &&
            std::none_of(occupied.begin(), occupied.end(), [&](const LabelBox& other) {
                return Overlaps(box, other);
            });
    };

    // Prefer a short leader from the true position; move the label, never the dot.
    for (int offset : {14, 38, 64, 94, 130}) {
        const LabelBox candidates[] = {{point.x + offset, point.y - height / 2, width, height},
            {point.x - offset - width, point.y - height / 2, width, height},
            {point.x - width / 2, point.y - offset - height, width, height},
            {point.x - width / 2, point.y + offset, width, height},
            {point.x + offset, point.y - offset - height, width, height},
            {point.x - offset - width, point.y + offset, width, height}};
        for (const auto& candidate : candidates) {
            if (fits(candidate)) {
                return candidate;
            }
        }
    }

    // Crowded junction: find the nearest free space instead of overwriting a label.
    std::optional<LabelBox> best;
    double best_distance = 1e30;
    for (int y = kMapTop + 6; y + height < kMapTop + kMapHeight - 3; y += 12) {
        for (int x = kMapLeft + 6; x + width < kMapLeft + kMapWidth - 3; x += 12) {
            const LabelBox candidate{x, y, width, height};
            const double dx = x + width / 2 - point.x;
            const double dy = y + height / 2 - point.y;
            if (fits(candidate) && dx * dx + dy * dy < best_distance) {
                best = candidate;
                best_distance = dx * dx + dy * dy;
            }
        }
    }
    return best;
}

struct Marker {
    ScreenPoint point;
    LabelBox box;
    std::string label;
};

// Stable ordering prevents label placement from changing with feed order.
std::vector<const Vehicle*> VisibleVehicles(
    const LiveMap& live_map, const MapProjection& projection) {
    std::vector<const Vehicle*> visible_vehicles;
    for (const auto& vehicle : live_map.vehicles) {
        if (projection.InMap(vehicle.position)) {
            visible_vehicles.push_back(&vehicle);
        }
    }

    std::sort(
        visible_vehicles.begin(), visible_vehicles.end(), [](const Vehicle* a, const Vehicle* b) {
            if (a->line != b->line) {
                return a->line < b->line;
            }
            if (a->position.latitude != b->position.latitude) {
                return a->position.latitude > b->position.latitude;
            }
            return a->position.longitude < b->position.longitude;
        });

    return visible_vehicles;
}

void DrawMapHeader(MonochromeCanvas& canvas, const LiveMap& live_map) {
    const std::string local_time = LocalServerTime(live_map.server_time);
    const std::string clock = DisplayTime(local_time);
    const int clock_left = 780 - MonochromeCanvas::TextWidth(clock, 3);
    canvas.DrawText(clock_left, 14, clock, 3);

    const auto station = live_map.nearest_station
        ? live_map.nearest_station
        : NearestStation(live_map.stops, live_map.config.center);
    const auto name = DisplayLabel(station ? station->name : "NO NEARBY STATION");
    const int title_width = clock_left - 40;
    if (MonochromeCanvas::TextWidth(name, 3) <= title_width) {
        canvas.DrawText(20, 14, name, 3);
    } else {
        // Longer provider names wrap into the former subtitle area, without
        // covering the clock. Extremely long names end with an ellipsis.
        const auto limit =
            static_cast<std::size_t>(title_width / MonochromeCanvas::TextWidth("M", 2));
        auto split = name.size() > limit ? name.rfind(' ', limit) : name.size();
        if (split == std::string::npos || split == 0) {
            split = limit;
        }
        canvas.DrawText(20, 14, name.substr(0, split), 2);
        const auto rest = name.find_first_not_of(' ', split);
        if (rest != std::string::npos) {
            canvas.DrawText(20, 44, FitLabel(name.substr(rest), title_width, 2), 2);
        }
    }

    const std::string date = DisplayDate(local_time);
    canvas.DrawText(780 - MonochromeCanvas::TextWidth(date, 2), 44, date, 2);
    canvas.DrawThickLine(20, 70, 780, 70, 3);
}

void DrawStreet(MonochromeCanvas& canvas, const Highway& street, int width, bool black,
    const MapProjection& projection) {
    for (std::size_t index = 1; index < street.points.size(); ++index) {
        LocalPoint from = projection.Localise(street.points[index - 1]);
        LocalPoint to = projection.Localise(street.points[index]);
        if (!ClipSegment(from, to)) {
            continue;
        }

        // Clip before converting to pixels, including ways crossing the entire view.
        const auto start = ProjectLocal(from);
        const auto end = ProjectLocal(to);
        canvas.DrawThickLine(start.x, start.y, end.x, end.y, width, black);
    }
}

void DrawFootpath(MonochromeCanvas& canvas, const Highway& path, const MapProjection& projection) {
    constexpr double dash_length = 4.0;
    constexpr double period = 8.0;
    double phase = 0.0;

    for (std::size_t index = 1; index < path.points.size(); ++index) {
        const LocalPoint original = projection.Localise(path.points[index - 1]);
        LocalPoint from = original;
        LocalPoint to = projection.Localise(path.points[index]);
        const double length = std::hypot(to.x - from.x, to.y - from.y) * kPixelsPerMetre;

        if (length > 0.0 && ClipSegment(from, to)) {
            const auto start = ProjectLocal(from);
            const auto end = ProjectLocal(to);
            const int steps = std::max(std::abs(end.x - start.x), std::abs(end.y - start.y));
            const double skipped =
                std::hypot(from.x - original.x, from.y - original.y) * kPixelsPerMetre;
            const double visible = std::hypot(to.x - from.x, to.y - from.y) * kPixelsPerMetre;

            // Carry the pattern through bends and clipped sections. Short OSM
            // segments must not restart every dash or turn the path into a solid line.
            for (int step = 0; step <= steps; ++step) {
                const double fraction = steps == 0 ? 0.0 : static_cast<double>(step) / steps;
                // Absorb coordinate roundoff at exact dash boundaries.
                if (std::fmod(phase + skipped + visible * fraction + 1e-6, period) < dash_length) {
                    const int x =
                        static_cast<int>(std::lround(start.x + (end.x - start.x) * fraction));
                    const int y =
                        static_cast<int>(std::lround(start.y + (end.y - start.y) * fraction));
                    canvas.SetPixel(x, y);
                }
            }
        }
        phase = std::fmod(phase + length, period);
    }
}

void DrawHighways(MonochromeCanvas& canvas, const std::vector<Highway>& highways,
    const MapProjection& projection) {
    // Outline streets with a white centre. Painting all borders before all
    // interiors keeps adjoining streets open at junctions, regardless of order.
    for (const bool border : {true, false}) {
        for (const auto& highway : highways) {
            if (!highway.footpath) {
                DrawStreet(canvas, highway, border ? 5 : 3, border, projection);
            }
        }
    }
    for (const auto& highway : highways) {
        if (highway.footpath) {
            DrawFootpath(canvas, highway, projection);
        }
    }
}

void DrawBuildings(
    MonochromeCanvas& canvas, const BuildingOutlines& outlines, const MapProjection& projection) {
    // Unfilled, one-pixel building outlines form the background. Draw first so
    // bold rails, platforms, leaders and opaque labels remain in the foreground.
    for (const auto& outline : outlines) {
        for (std::size_t index = 1; index < outline.size(); ++index) {
            LocalPoint from = projection.Localise(outline[index - 1]);
            LocalPoint to = projection.Localise(outline[index]);
            if (!ClipSegment(from, to)) {
                continue;
            }

            canvas.DrawLine(static_cast<int>(std::lround(kMapCenterX + from.x * kPixelsPerMetre)),
                static_cast<int>(std::lround(kMapCenterY - from.y * kPixelsPerMetre)),
                static_cast<int>(std::lround(kMapCenterX + to.x * kPixelsPerMetre)),
                static_cast<int>(std::lround(kMapCenterY - to.y * kPixelsPerMetre)));
        }
    }
}

void DrawTracks(
    MonochromeCanvas& canvas, const LiveMap& live_map, const MapProjection& projection) {
    // Draw the mapped rail curves, independent of which trips are on the board.
    for (std::size_t way = 0; way < live_map.tracks.size(); ++way) {
        const auto& track = live_map.tracks[way];
        const int stroke = way < live_map.track_stroke_widths.size()
            ? std::clamp(live_map.track_stroke_widths[way], 1, 5)
            : 5;

        for (std::size_t index = 0; index + 1 < track.size(); ++index) {
            LocalPoint from = projection.Localise(track[index]);
            LocalPoint to = projection.Localise(track[index + 1]);
            if (!ClipSegment(from, to)) {
                continue;
            }

            canvas.DrawThickLine(
                static_cast<int>(std::lround(kMapCenterX + from.x * kPixelsPerMetre)),
                static_cast<int>(std::lround(kMapCenterY - from.y * kPixelsPerMetre)),
                static_cast<int>(std::lround(kMapCenterX + to.x * kPixelsPerMetre)),
                static_cast<int>(std::lround(kMapCenterY - to.y * kPixelsPerMetre)), stroke);
        }
    }
}

void DrawStations(MonochromeCanvas& canvas, const std::vector<Stop>& stops,
    std::vector<LabelBox>& occupied, const MapProjection& projection) {
    // Hollow circles distinguish the station platforms from the solid train dots.
    for (const Stop& stop : stops) {
        if (!projection.InMap(stop.position)) {
            continue;
        }

        const ScreenPoint point = projection.Project(stop.position);
        canvas.FillCircle(point.x, point.y, 9, false);
        canvas.FillCircle(point.x, point.y, 7);
        canvas.FillCircle(point.x, point.y, 4, false);
        occupied.push_back({point.x - 10, point.y - 10, 20, 20});
    }
}

// Reserve static annotations before placing moving train badges.
std::vector<Marker> LayoutPlaces(const std::vector<PlaceMarker>& places,
    std::vector<LabelBox>& occupied, const MapProjection& projection) {
    constexpr int height = 24;
    std::vector<Marker> markers;
    std::vector<LabelBox> reserved;
    for (const auto& place : places) {
        if (projection.InMap(place.position)) {
            const auto point = projection.Project(place.position);
            markers.push_back(
                {point, {}, FitLabel(DisplayLabel(place.label), kMapWidth - 18, kPlaceLabelScale)});
            reserved.push_back({point.x - 13, point.y - 13, 26, 26});
        }
    }

    // Only other configured places affect these labels. Changes in the live
    // station catalog or moving trains must not rearrange a permanent annotation.
    for (std::size_t index = 0; index < markers.size(); ++index) {
        auto& marker = markers[index];
        if (marker.label.empty()) {
            continue;
        }
        const auto point = marker.point;
        const int width = MonochromeCanvas::TextWidth(marker.label, kPlaceLabelScale) + 12;
        const LabelBox preferred{
            std::clamp(point.x - width / 2, kMapLeft + 3, kMapLeft + kMapWidth - width - 3),
            std::clamp(point.y - 18 - height, kMapTop + 3, kMapTop + kMapHeight - height - 3),
            width, height};
        bool overlaps = false;
        for (std::size_t other = 0; other < reserved.size(); ++other) {
            if (other == index) {
                // The preferred north placement has a deliberately short leader;
                // test its actual box rather than adding the general label gap.
                const auto& symbol = reserved[other];
                overlaps = overlaps ||
                    (preferred.x < symbol.x + symbol.width &&
                        preferred.x + preferred.width > symbol.x &&
                        preferred.y < symbol.y + symbol.height &&
                        preferred.y + preferred.height > symbol.y);
            } else {
                overlaps = overlaps || Overlaps(preferred, reserved[other]);
            }
        }
        const auto box = overlaps ? PlaceLabel(point, width, reserved, height)
                                  : std::optional<LabelBox>(preferred);
        if (box) {
            marker.box = *box;
            reserved.push_back(*box);
        } else {
            // Keep the geographic symbol even when no label fits.
            marker.label.clear();
        }
    }
    occupied.insert(occupied.end(), reserved.begin(), reserved.end());
    return markers;
}

// Keep both station symbols and train dots clear when placing labels.
std::vector<Marker> LayoutVehicleLabels(const std::vector<const Vehicle*>& visible_vehicles,
    std::vector<LabelBox>& occupied, const MapProjection& projection) {
    for (const Vehicle* vehicle : visible_vehicles) {
        const ScreenPoint point = projection.Project(vehicle->position);
        occupied.push_back({point.x - 8, point.y - 8, 16, 16});
    }

    std::vector<Marker> markers;
    for (const Vehicle* vehicle : visible_vehicles) {
        const ScreenPoint point = projection.Project(vehicle->position);
        const std::string label = VehicleLabel(vehicle->line);
        const int width = std::max(40, MonochromeCanvas::TextWidth(label, 3) + 14);
        if (const auto box = PlaceLabel(point, width, occupied)) {
            occupied.push_back(*box);
            markers.push_back({point, *box, label});
        }
    }

    return markers;
}

void DrawVehicles(MonochromeCanvas& canvas, const std::vector<const Vehicle*>& visible_vehicles,
    const std::vector<Marker>& markers, const MapProjection& projection) {
    // All leaders go behind all badges, so a later train cannot cut through text.
    for (const auto& marker : markers) {
        const int end_x =
            std::clamp(marker.point.x, marker.box.x, marker.box.x + marker.box.width - 1);
        const int end_y =
            std::clamp(marker.point.y, marker.box.y, marker.box.y + marker.box.height - 1);
        canvas.DrawThickLine(marker.point.x, marker.point.y, end_x, end_y, 5, false);
        canvas.DrawThickLine(marker.point.x, marker.point.y, end_x, end_y, 2);
    }

    for (const Vehicle* vehicle : visible_vehicles) {
        const ScreenPoint point = projection.Project(vehicle->position);
        canvas.FillCircle(point.x, point.y, 8, false);
        canvas.FillCircle(point.x, point.y, 5);
    }

    for (const auto& marker : markers) {
        const auto& box = marker.box;
        canvas.FillRect(box.x - 3, box.y - 3, box.width + 6, box.height + 6, false);
        canvas.FillRect(box.x, box.y, box.width, box.height);
        canvas.DrawText(box.x + 8, box.y + 6, marker.label, 3, false);
    }
}

void DrawPlaces(MonochromeCanvas& canvas, const std::vector<Marker>& markers) {
    for (const auto& marker : markers) {
        if (!marker.label.empty()) {
            const auto& box = marker.box;
            const int end_x = std::clamp(marker.point.x, box.x, box.x + box.width - 1);
            const int end_y = std::clamp(marker.point.y, box.y, box.y + box.height - 1);
            canvas.DrawThickLine(marker.point.x, marker.point.y, end_x, end_y, 5, false);
            canvas.DrawThickLine(marker.point.x, marker.point.y, end_x, end_y, 2);
        }
    }
    for (const auto& marker : markers) {
        if (!marker.label.empty()) {
            const auto& box = marker.box;
            canvas.FillRect(box.x - 3, box.y - 3, box.width + 6, box.height + 6, false);
            canvas.FillRect(box.x, box.y, box.width, box.height);
            canvas.FillRect(box.x + 1, box.y + 1, box.width - 2, box.height - 2, false);
            canvas.DrawText(box.x + 6, box.y + 5, marker.label, kPlaceLabelScale);
        }
    }
    // Draw after all leaders and labels so every geographic symbol stays visible.
    for (const auto& marker : markers) {
        const int x = marker.point.x;
        const int y = marker.point.y;
        canvas.FillCircle(x, y, 13, false);
        canvas.DrawThickLine(x, y - 10, x + 10, y, 3);
        canvas.DrawThickLine(x + 10, y, x, y + 10, 3);
        canvas.DrawThickLine(x, y + 10, x - 10, y, 3);
        canvas.DrawThickLine(x - 10, y, x, y - 10, 3);
        canvas.FillCircle(x, y, 2);
    }
}

void DrawMapLegend(MonochromeCanvas& canvas) {
    constexpr int right_margin = 20;
    constexpr int text_scale = 2;
    constexpr int train_marker_width = 38;
    constexpr int train_text_gap = 13;
    constexpr int item_gap = 28;
    constexpr int platform_text_offset = 16;

    // TextWidth includes trailing spacing; align the visible glyphs to the right.
    const int train_text_x =
        EPD_WIDTH - right_margin - (MonochromeCanvas::TextWidth("TRAIN", text_scale) - text_scale);
    const int train_marker_x = train_text_x - train_text_gap - train_marker_width;
    const int platform_text_x =
        train_marker_x - item_gap - MonochromeCanvas::TextWidth("PLATFORM", text_scale);
    const int platform_marker_x = platform_text_x - platform_text_offset;

    canvas.DrawThickLine(20, 446, 780, 446, 2);

    canvas.FillCircle(platform_marker_x, 466, 7);
    canvas.FillCircle(platform_marker_x, 466, 4, false);
    canvas.DrawText(platform_text_x, 459, "PLATFORM", text_scale);

    canvas.FillRect(train_marker_x, 455, train_marker_width, 23);
    canvas.DrawText(train_marker_x + 7, 460, "S2", text_scale, false);
    canvas.DrawText(train_text_x, 459, "TRAIN", text_scale);
}

void DrawLoadingScreen(
    MonochromeCanvas& canvas, bool map_ready, bool transit_ready, const MapScreenStatus& status) {
    // Clear the entire previous frame, including any active map clipping.
    canvas.ResetClip();
    canvas.Clear();

    const auto centered_text = [&](int y, const std::string& text, int scale) {
        canvas.DrawText((800 - MonochromeCanvas::TextWidth(text, scale)) / 2, y, text, scale);
    };
    const auto progress_row = [&](int y, const std::string& label, bool ready, bool failed) {
        const std::string state = ready ? "READY" : (failed ? "RETRYING" : "LOADING");
        canvas.DrawText(230, y, label, 2);
        canvas.DrawText(570 - MonochromeCanvas::TextWidth(state, 2), y, state, 2);
    };

    centered_text(146, "LOADING MAP", 4);
    canvas.DrawLine(200, 198, 600, 198);
    progress_row(228, "MAP", map_ready, status.map_failed);
    progress_row(268, "DEPARTURES", transit_ready, status.transit_failed);

    // Show a retry hint only for a source that still prevents initial display.
    const bool waiting_for_retry =
        (!map_ready && status.map_failed) || (!transit_ready && status.transit_failed);
    if (waiting_for_retry) {
        centered_text(324, "CONNECTION PROBLEM - RETRYING AUTOMATICALLY", 1);
    } else {
        centered_text(324, "PLEASE WAIT", 2);
    }
}

} // namespace

osm::BoundingBox MapViewport(const MapConfig& config) {
    const auto bounds = MapProjection(config.center).Bounds();
    bounds.Validate();
    return bounds;
}

std::optional<Stop> NearestStation(const std::vector<Stop>& stops, GeoPoint center) {
    std::optional<Stop> nearest;
    double best = std::numeric_limits<double>::infinity();
    for (const auto& stop : stops) {
        const auto point = stop.position;
        if (stop.name.find_first_not_of(" \t\r\n") == std::string::npos ||
            !std::isfinite(point.latitude) || !std::isfinite(point.longitude) ||
            std::abs(point.latitude) > 90.0 || std::abs(point.longitude) > 180.0) {
            continue;
        }
        // Haversine's angular-distance term also works away from Karlsruhe.
        const double latitude =
            std::sin((point.latitude - center.latitude) * kRadiansPerDegree / 2);
        const double longitude =
            std::sin((point.longitude - center.longitude) * kRadiansPerDegree / 2);
        const double distance = latitude * latitude +
            longitude * longitude * std::cos(center.latitude * kRadiansPerDegree) *
                std::cos(point.latitude * kRadiansPerDegree);
        if (distance < best || (distance == best && nearest && stop.id < nearest->id)) {
            best = distance;
            nearest = stop;
        }
    }
    return nearest;
}

MapBackground::MapBackground(const osm::MapData& data) : rails(data.rails) {
    for (const auto& outline : data.buildings) {
        std::vector<GeoPoint> points;
        for (const auto& point : outline) {
            points.push_back({point.latitude, point.longitude});
        }
        buildings.push_back(std::move(points));
    }

    for (const auto& way : data.highways) {
        Highway highway;
        highway.footpath = way.kind == "footway" || way.kind == "path" || way.kind == "steps" ||
            way.kind == "pedestrian" || way.kind == "cycleway" || way.kind == "bridleway";
        for (const auto& point : way.points) {
            highway.points.push_back({point.latitude, point.longitude});
        }
        highways.push_back(std::move(highway));
    }

    for (const auto& stop : data.stops) {
        std::string id = "osm:" + std::to_string(stop.id);
        // Karlsruhe's IFOPT stop number maps to the local EFA ID. Keep this
        // provider-specific association in the app, outside the OSM library.
        constexpr std::string_view prefix = "de:08212:";
        if (stop.ifopt.compare(0, prefix.size(), prefix) == 0) {
            const auto end = stop.ifopt.find(':', prefix.size());
            const auto number =
                std::string_view(stop.ifopt)
                    .substr(prefix.size(),
                        end == std::string::npos ? std::string::npos : end - prefix.size());
            int local = 0;
            const auto parsed =
                std::from_chars(number.data(), number.data() + number.size(), local);
            if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() &&
                local > 0 && local < 1000000) {
                id = std::to_string(7000000 + local);
            }
        }
        stops.push_back({id, stop.name, {stop.position.latitude, stop.position.longitude}});
    }
}

LiveMap MakeLiveMap(
    const TransitSnapshot& snapshot, const MapBackground& background, const MapConfig& config) {
    const auto& rails = background.rails;
    LiveMap map;
    map.config = config;
    const MapProjection projection(config.center);

    // Carry freshness information alongside the static background.
    map.server_time = snapshot.server_time;
    map.stale = snapshot.stale;
    map.replay = snapshot.replay;
    map.age_seconds = snapshot.age_seconds;
    map.buildings = background.buildings;
    map.highways = background.highways;
    map.tracks = rails.Tracks();
    map.track_stroke_widths = rails.StrokeWidths();

    // Compare every known platform for the header, even without departures.
    // A distant live platform must not hide a closer OSM platform at that stop.
    auto catalog = snapshot.stops;
    catalog.insert(catalog.end(), background.stops.begin(), background.stops.end());
    map.nearest_station = NearestStation(catalog, config.center);

    // For drawing, visible live platforms take precedence over OSM stations.
    std::set<std::string> observed_station_ids;
    for (const auto& stop : snapshot.stops) {
        if (projection.InMap(stop.position)) {
            map.stops.push_back(stop);
            observed_station_ids.insert(stop.id);
        }
    }
    for (const auto& stop : background.stops) {
        if (projection.InMap(stop.position) && !observed_station_ids.count(stop.id)) {
            map.stops.push_back(stop);
        }
    }

    // Convert timetable progress to a point on the connected rail network.
    for (const auto& vehicle : snapshot.vehicles) {
        const bool is_dwelling = vehicle.from.latitude == vehicle.to.latitude &&
            vehicle.from.longitude == vehicle.to.longitude;
        const auto position = is_dwelling
            ? rails.Snap(vehicle.from)
            : rails.Interpolate(vehicle.from, vehicle.to, vehicle.progress);
        if (position && projection.InMap(*position)) {
            map.vehicles.push_back({vehicle.line, vehicle.destination, *position});
        }
    }

    return map;
}

void DrawMapScreen(MonochromeCanvas& canvas, const std::optional<TransitSnapshot>& snapshot,
    const MapBackground* background, const MapScreenStatus& status, const MapConfig& config) {
    if (!snapshot || !background) {
        DrawLoadingScreen(canvas, background != nullptr, snapshot.has_value(), status);
        return;
    }

    // Both sources retain successful results while refreshing in the background.
    auto map = MakeLiveMap(*snapshot, *background, config);
    if (status.map_failed) {
        map.map_status = "MAP CACHED - RETRYING";
    } else if (status.map_loading) {
        map.map_status = "UPDATING MAP";
    }
    DrawLiveMap(canvas, map);
}

void DrawLiveMap(MonochromeCanvas& canvas, const LiveMap& live_map) {
    canvas.ResetClip();
    canvas.Clear();

    const MapProjection projection(live_map.config.center);
    const auto visible_vehicles = VisibleVehicles(live_map, projection);
    DrawMapHeader(canvas, live_map);

    // Clip geographic features before adding the labels above them.
    canvas.SetClipRect(kMapLeft, kMapTop, kMapWidth, kMapHeight);
    DrawHighways(canvas, live_map.highways, projection);
    DrawBuildings(canvas, live_map.buildings, projection);
    DrawTracks(canvas, live_map, projection);

    std::vector<LabelBox> occupied;
    DrawStations(canvas, live_map.stops, occupied, projection);

    const auto places = LayoutPlaces(live_map.config.places, occupied, projection);
    const auto markers = LayoutVehicleLabels(visible_vehicles, occupied, projection);
    DrawVehicles(canvas, visible_vehicles, markers, projection);

    // Permanent annotations are painted last so moving markers cannot hide them.
    DrawPlaces(canvas, places);
    // Network state belongs in the UI; background geometry survives outages.
    if (!live_map.map_status.empty()) {
        const int width = MonochromeCanvas::TextWidth(live_map.map_status, 1) + 12;
        canvas.FillRect(kMapLeft + kMapWidth - width, kMapTop + 4, width, 17, false);
        canvas.DrawText(kMapLeft + kMapWidth - width + 6, kMapTop + 9, live_map.map_status, 1);
    }
    canvas.ResetClip();
    DrawMapLegend(canvas);
}

void DrawLiveMapError(MonochromeCanvas& canvas, const std::string& error) {
    canvas.ResetClip();
    canvas.Clear();
    canvas.DrawText(24, 24, "LIVE TRANSIT MAP", 2);
    canvas.DrawLine(24, 55, 776, 55);

    // Keep the connection failure separate from the underlying provider error.
    canvas.DrawText(24, 82, "LIVE DATA COULD NOT BE READ", 2);
    canvas.DrawText(24, 118, "CHECK INTERNET CONNECTION AND TRY AGAIN", 1);
    canvas.DrawText(24, 138, "KVV EFA: " + error.substr(0, 100), 1);

    canvas.DrawText(24, 445, "THE DISPLAY API IS STILL THE WAVESHARE EPD API", 1);
}

} // namespace kvv
