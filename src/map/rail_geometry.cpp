#include "rail_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>

namespace kvv {
namespace {

// Use one local metric for edge lengths, point projection and interpolation.
constexpr double kLatitudeScale = 111320.0;

GeoPoint Lerp(GeoPoint a, GeoPoint b, double fraction) {
    return {a.latitude + (b.latitude - a.latitude) * fraction,
        a.longitude + (b.longitude - a.longitude) * fraction};
}

} // namespace

double RailGeometry::Distance(GeoPoint a, GeoPoint b) const {
    return std::hypot(
        (a.longitude - b.longitude) * longitude_scale_, (a.latitude - b.latitude) * kLatitudeScale);
}

RailGeometry::RailGeometry(const std::vector<osm::RailWay>& ways)
    : longitude_scale_(kLatitudeScale) {
    double south = std::numeric_limits<double>::infinity();
    double north = -std::numeric_limits<double>::infinity();
    for (const auto& way : ways) {
        for (const auto& node : way.nodes) {
            south = std::min(south, node.position.latitude);
            north = std::max(north, node.position.latitude);
        }
    }
    if (south <= north) {
        const double middle_latitude = (south + north) * 0.5;
        longitude_scale_ = kLatitudeScale * std::cos(middle_latitude * 3.141592653589793 / 180.0);
    }

    std::map<std::int64_t, int> node_ids;

    // The OSM library has already split incomplete ways. This application layer
    // chooses stroke widths and builds the graph used for transit estimates.
    for (const auto& way : ways) {
        const auto& service = way.service;
        const int stroke = service == "yard" || service == "siding" || service == "spur" ? 1 : 5;
        std::vector<GeoPoint> track;
        int previous = -1;

        for (const auto& node : way.nodes) {
            const GeoPoint point{node.position.latitude, node.position.longitude};
            const auto [entry, inserted] = node_ids.emplace(node.id, nodes_.size());
            const int index = entry->second;
            if (inserted) {
                nodes_.push_back(point);
                neighbours_.emplace_back();
            }

            // Shared OSM IDs join ways; nearby but distinct nodes stay separate.
            track.push_back(point);
            if (previous >= 0 && previous != index) {
                const double length = Distance(nodes_[previous], point);
                if (length > 0.01) {
                    edges_.push_back({previous, index, length});
                    neighbours_[previous].push_back({index, length});
                    neighbours_[index].push_back({previous, length});
                }
            }
            previous = index;
        }

        if (track.size() > 1) {
            tracks_.push_back(std::move(track));
            stroke_widths_.push_back(stroke);
        }
    }
}

std::optional<RailGeometry::Match> RailGeometry::MatchPoint(GeoPoint point) const {
    double best = 80.0; // Do not place a stop on a distant, unrelated corridor.
    std::optional<Match> match;

    // Project onto each segment and retain the nearest admissible match.
    for (std::size_t i = 0; i < edges_.size(); ++i) {
        const auto& edge = edges_[i];
        const auto a = nodes_[edge.from];
        const auto b = nodes_[edge.to];
        const double dx = (b.longitude - a.longitude) * longitude_scale_;
        const double dy = (b.latitude - a.latitude) * kLatitudeScale;

        const double fraction =
            std::clamp(((point.longitude - a.longitude) * longitude_scale_ * dx +
                           (point.latitude - a.latitude) * kLatitudeScale * dy) /
                    (edge.length * edge.length),
                0.0, 1.0);

        const GeoPoint projected = Lerp(a, b, fraction);
        const double distance = Distance(point, projected);
        if (distance < best) {
            best = distance;
            match = Match{static_cast<int>(i), fraction, projected};
        }
    }

    return match;
}

std::optional<GeoPoint> RailGeometry::Snap(GeoPoint point) const {
    const auto match = MatchPoint(point);
    return match ? std::optional<GeoPoint>(match->point) : std::nullopt;
}

std::optional<GeoPoint> RailGeometry::Interpolate(
    GeoPoint from, GeoPoint to, double progress) const {
    const auto start = MatchPoint(from);
    const auto finish = MatchPoint(to);
    if (!start || !finish) {
        return std::nullopt;
    }

    // Stops on the same edge need no graph search.
    progress = std::clamp(progress, 0.0, 1.0);
    if (start->edge == finish->edge) {
        return Lerp(start->point, finish->point, progress);
    }

    // Seed both ends of the starting segment with their partial-edge distances.
    const auto& start_edge = edges_[start->edge];
    const auto& finish_edge = edges_[finish->edge];
    const double infinity = std::numeric_limits<double>::infinity();
    std::vector<double> distances(nodes_.size(), infinity);
    std::vector<int> previous(nodes_.size(), -1);
    using Candidate = std::pair<double, int>;
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> queue;
    distances[start_edge.from] = start_edge.length * start->fraction;
    distances[start_edge.to] = start_edge.length * (1.0 - start->fraction);
    queue.push({distances[start_edge.from], start_edge.from});
    queue.push({distances[start_edge.to], start_edge.to});

    // Dijkstra's search records the shortest connected path through the rails.
    while (!queue.empty()) {
        const auto [distance, node] = queue.top();
        queue.pop();
        if (distance != distances[node]) {
            continue;
        }
        for (const auto& [next, length] : neighbours_[node]) {
            if (distance + length < distances[next]) {
                distances[next] = distance + length;
                previous[next] = node;
                queue.push({distances[next], next});
            }
        }
    }

    // Account for the final partial edge from either of its endpoints.
    const double via_from = distances[finish_edge.from] + finish_edge.length * finish->fraction;
    const double via_to = distances[finish_edge.to] + finish_edge.length * (1.0 - finish->fraction);
    const double length = std::min(via_from, via_to);

    // Reject disconnected tracks and implausible detours. This is a geometric
    // estimate, not an assertion of the operator's route or chosen track.
    if (!std::isfinite(length) || length > 3.0 * Distance(from, to) + 200.0) {
        return std::nullopt;
    }

    // Follow predecessor links backward, then put the polyline in travel order.
    int node = via_from <= via_to ? finish_edge.from : finish_edge.to;
    std::vector<GeoPoint> path{finish->point};
    while (node >= 0) {
        path.push_back(nodes_[node]);
        node = previous[node];
    }
    path.push_back(start->point);
    std::reverse(path.begin(), path.end());

    // Walk the requested fraction of route length, preserving all mapped bends.
    double remaining = progress * length;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const double segment = Distance(path[i - 1], path[i]);
        if (segment > 0.0 && remaining <= segment) {
            return Lerp(path[i - 1], path[i], remaining / segment);
        }
        remaining -= segment;
    }

    return finish->point;
}

} // namespace kvv
