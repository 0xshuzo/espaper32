#pragma once

#include "kvv_efa_query.h"
#include <osm_query_parser.h>
#include <optional>
#include <string>
#include <vector>

namespace kvv {

// OSM track nodes, not stop-to-stop chords. Shared OSM node IDs define junctions.
class RailGeometry {
public:
    explicit RailGeometry(const std::vector<osm::RailWay>& ways = {});

    // Drawing uses the original polylines; routing uses the graph built from them.
    const std::vector<std::vector<GeoPoint>>& Tracks() const {
        return tracks_;
    }
    const std::vector<int>& StrokeWidths() const {
        return stroke_widths_;
    }

    // Unmatched or disconnected coordinates produce no estimated position.
    std::optional<GeoPoint> Snap(GeoPoint point) const;
    std::optional<GeoPoint> Interpolate(GeoPoint from, GeoPoint to, double progress) const;

private:
    struct Edge {
        int from, to;
        double length;
    };

    struct Match {
        int edge;
        double fraction;
        GeoPoint point;
    };

    double Distance(GeoPoint a, GeoPoint b) const;
    std::optional<Match> MatchPoint(GeoPoint point) const;

    double longitude_scale_;
    std::vector<GeoPoint> nodes_;
    std::vector<Edge> edges_;
    std::vector<std::vector<std::pair<int, double>>> neighbours_;
    std::vector<std::vector<GeoPoint>> tracks_;
    std::vector<int> stroke_widths_;
};

} // namespace kvv
