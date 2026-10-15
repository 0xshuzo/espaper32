include_guard(GLOBAL)

# Search only this project and explicitly supplied source trees. The environment
# form uses the same semicolon-separated list as the CMake cache variable.
if(NOT DEFINED SOURCE_DEPENDENCY_ROOTS)
  set(SOURCE_DEPENDENCY_ROOTS "" CACHE STRING "Semicolon-separated source dependency search roots")
endif()

function(_source_dependency_has_capability directory capability result)
  set(_matches FALSE)
  if(EXISTS "${directory}/CMakeLists.txt" AND
     NOT IS_DIRECTORY "${directory}/CMakeLists.txt" AND
     EXISTS "${directory}/cmake/capabilities.txt" AND
     NOT IS_DIRECTORY "${directory}/cmake/capabilities.txt")
    file(STRINGS "${directory}/cmake/capabilities.txt" _capabilities)
    foreach(_entry IN LISTS _capabilities)
      string(STRIP "${_entry}" _entry)
      if(_entry STREQUAL capability)
        set(_matches TRUE)
        break()
      endif()
    endforeach()
  endif()
  set("${result}" "${_matches}" PARENT_SCOPE)
endfunction()

function(_source_dependency_scan directory capability visited_name matches_name)
  get_filename_component(_directory "${directory}" REALPATH)
  get_filename_component(_directory_name "${_directory}" NAME)
  set(_visited "${${visited_name}}")
  set(_matches "${${matches_name}}")
  list(FIND _visited "${_directory}" _visited_index)
  # A source directory can also contain its own in-source build cache. Skip
  # generated build trees, but keep actual source roots discoverable.
  if(NOT _visited_index EQUAL -1 OR _directory_name MATCHES "^\\." OR
     (EXISTS "${_directory}/CMakeCache.txt" AND
      (NOT EXISTS "${_directory}/CMakeLists.txt" OR
       IS_DIRECTORY "${_directory}/CMakeLists.txt")))
    return()
  endif()
  list(APPEND _visited "${_directory}")

  _source_dependency_has_capability("${_directory}" "${capability}" _provides)
  if(_provides)
    list(APPEND _matches "${_directory}")
  endif()

  # Checkout paths are literal, even when their names contain glob characters.
  string(REGEX REPLACE "([][?*])" "[\\1]" _glob_directory "${_directory}")
  file(GLOB _children LIST_DIRECTORIES TRUE "${_glob_directory}/*")
  foreach(_child IN LISTS _children)
    get_filename_component(_name "${_child}" NAME)
    if(IS_DIRECTORY "${_child}" AND NOT _name MATCHES "^\\.")
      _source_dependency_scan("${_child}" "${capability}" _visited _matches)
    endif()
  endforeach()
  set("${visited_name}" "${_visited}" PARENT_SCOPE)
  set("${matches_name}" "${_matches}" PARENT_SCOPE)
endfunction()

function(add_source_dependency)
  cmake_parse_arguments(PARSE_ARGV 0 DEPENDENCY ""
    "TARGET;CAPABILITY;DESCRIPTION;OVERRIDE" "")
  foreach(_required TARGET CAPABILITY DESCRIPTION OVERRIDE)
    if(NOT DEFINED DEPENDENCY_${_required} OR "${DEPENDENCY_${_required}}" STREQUAL "")
      message(FATAL_ERROR "add_source_dependency requires ${_required}.")
    endif()
  endforeach()
  if(DEPENDENCY_UNPARSED_ARGUMENTS OR
     NOT DEPENDENCY_CAPABILITY MATCHES "^[a-z0-9][a-z0-9-]*$" OR
     NOT DEPENDENCY_OVERRIDE MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "Invalid add_source_dependency arguments.")
  endif()
  if(TARGET "${DEPENDENCY_TARGET}")
    return()
  endif()

  if(NOT DEFINED ${DEPENDENCY_OVERRIDE})
    set("${DEPENDENCY_OVERRIDE}" "" CACHE PATH "Source directory for ${DEPENDENCY_DESCRIPTION}")
  endif()
  if(NOT "${${DEPENDENCY_OVERRIDE}}" STREQUAL "")
    get_filename_component(_provider "${${DEPENDENCY_OVERRIDE}}" ABSOLUTE
      BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(IS_DIRECTORY "${_provider}")
      get_filename_component(_provider "${_provider}" REALPATH)
      _source_dependency_has_capability("${_provider}" "${DEPENDENCY_CAPABILITY}" _provides)
    else()
      set(_provides FALSE)
    endif()
    if(NOT _provides)
      message(FATAL_ERROR
        "Invalid ${DEPENDENCY_OVERRIDE}: set it to a source directory providing ${DEPENDENCY_DESCRIPTION}.")
    endif()
  else()
    set(_roots "${CMAKE_SOURCE_DIR}")
    list(APPEND _roots ${SOURCE_DEPENDENCY_ROOTS})
    if(DEFINED ENV{SOURCE_DEPENDENCY_ROOTS})
      list(APPEND _roots "$ENV{SOURCE_DEPENDENCY_ROOTS}")
    endif()
    set(_canonical_roots "")
    foreach(_root IN LISTS _roots)
      if(_root STREQUAL "")
        continue()
      endif()
      get_filename_component(_root "${_root}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
      if(NOT IS_DIRECTORY "${_root}")
        message(FATAL_ERROR
          "Invalid SOURCE_DEPENDENCY_ROOTS: '${_root}' is not a directory; supply existing search roots.")
      endif()
      get_filename_component(_root "${_root}" REALPATH)
      list(APPEND _canonical_roots "${_root}")
    endforeach()
    list(REMOVE_DUPLICATES _canonical_roots)
    set(_visited "")
    set(_matches "")
    foreach(_root IN LISTS _canonical_roots)
      _source_dependency_scan("${_root}" "${DEPENDENCY_CAPABILITY}" _visited _matches)
    endforeach()
    list(REMOVE_DUPLICATES _matches)
    list(LENGTH _matches _count)
    if(_count EQUAL 0)
      message(FATAL_ERROR
        "Missing ${DEPENDENCY_DESCRIPTION}; set ${DEPENDENCY_OVERRIDE} or SOURCE_DEPENDENCY_ROOTS.")
    elseif(_count GREATER 1)
      string(REPLACE ";" ", " _locations "${_matches}")
      message(FATAL_ERROR
        "Ambiguous ${DEPENDENCY_DESCRIPTION}: ${_locations}; set ${DEPENDENCY_OVERRIDE} to the intended source directory.")
    endif()
    list(GET _matches 0 _provider)
  endif()

  add_subdirectory("${_provider}"
    "${CMAKE_CURRENT_BINARY_DIR}/_source_deps/${DEPENDENCY_CAPABILITY}")
  if(NOT TARGET "${DEPENDENCY_TARGET}")
    message(FATAL_ERROR
      "Invalid ${DEPENDENCY_DESCRIPTION} provider: missing target ${DEPENDENCY_TARGET}; check ${DEPENDENCY_OVERRIDE}.")
  endif()
endfunction()
