cmake_minimum_required(VERSION 3.16)
project(espaper32 LANGUAGES CXX)
include(cmake/SourceDependencies.cmake)

# Resolve SDL here so its imported target is also visible to application tests.
if(NOT TARGET SDL2::SDL2)
  find_package(SDL2 2.0 CONFIG QUIET)
endif()
if(NOT TARGET SDL2::SDL2)
  message(FATAL_ERROR "SDL2 development files not found. Install SDL2 or set SDL2_DIR.")
endif()

add_source_dependency(TARGET epd7in5_v2_api CAPABILITY display-api
  DESCRIPTION "Display interface" OVERRIDE DISPLAY_API_SOURCE_DIR)
add_source_dependency(TARGET kvv_efa_query CAPABILITY transit-query
  DESCRIPTION "Transit query library" OVERRIDE TRANSIT_QUERY_SOURCE_DIR)
add_source_dependency(TARGET osm_query_parser CAPABILITY map-query
  DESCRIPTION "Map query library" OVERRIDE MAP_QUERY_SOURCE_DIR)
add_source_dependency(TARGET epd7in5_v2_simulator CAPABILITY display-simulator
  DESCRIPTION "Display simulator" OVERRIDE DISPLAY_SIMULATOR_SOURCE_DIR)

add_library(espaper32_map
  src/monochrome_canvas.cpp src/map/kvv_live_map.cpp
  src/map/rail_geometry.cpp)
target_compile_features(espaper32_map PUBLIC cxx_std_17)
target_include_directories(espaper32_map PUBLIC src src/map)
target_link_libraries(espaper32_map PUBLIC kvv_efa_query osm_query_parser epd7in5_v2_api)
add_executable(epaper_simulator desktop/main.cpp)
target_link_libraries(epaper_simulator PRIVATE espaper32_map epd7in5_v2_simulator)
include(CTest)
if(BUILD_TESTING)
  add_executable(map_render_checks tests/map_render_checks.cpp)
  target_link_libraries(map_render_checks PRIVATE espaper32_map nlohmann_json::nlohmann_json SDL2::SDL2)
  add_test(NAME map_render_checks COMMAND map_render_checks)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Python 3 is required for dependency-discovery tests.")
  endif()
  add_test(NAME cmake_discovery_checks COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_discovery_checks.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SourceDependencies.cmake" "${CMAKE_COMMAND}")
endif()
