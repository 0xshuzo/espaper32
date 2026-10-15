#pragma once

#include "kvv_efa_query.h"
#include <string>
#include <vector>

namespace kvv {

struct PlaceMarker {
    GeoPoint position;
    std::string label;
};

// Edit these defaults to choose your map. Coordinates are WGS84 latitude,
// longitude, in decimal degrees. Rebuild after changing this file.
struct MapConfig {
    // Karlsruhe Hauptbahnhof (Vorplatz), from the EFA station coordinates.
    GeoPoint center{48.994369, 8.399670};

    // Optional fixed markers. Add any number of entries here.
    std::vector<PlaceMarker> places{
        // {{48.995000, 8.400000}, "MY PLACE"},
    };
};

} // namespace kvv
