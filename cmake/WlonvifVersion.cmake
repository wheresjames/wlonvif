include_guard(GLOBAL)

# PROJECT.txt is the single source of truth for project metadata, including the
# version. Every CMake project in this tree (the library and the installed-package
# consumer smoke test) reads it through this module rather than restating values.
#
# Each "key   value" line becomes WLONVIF_PROJECT_<KEY> in the caller's scope.
set(WLONVIF_PROJECT_FILE "${CMAKE_CURRENT_LIST_DIR}/../PROJECT.txt")
get_filename_component(WLONVIF_PROJECT_FILE "${WLONVIF_PROJECT_FILE}" ABSOLUTE)
if(NOT EXISTS "${WLONVIF_PROJECT_FILE}")
  message(FATAL_ERROR "Project metadata file not found: ${WLONVIF_PROJECT_FILE}")
endif()

# Re-run configuration whenever the metadata changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WLONVIF_PROJECT_FILE}")

file(STRINGS "${WLONVIF_PROJECT_FILE}" _wlonvif_project_lines)
foreach(_wlonvif_line IN LISTS _wlonvif_project_lines)
  if(_wlonvif_line MATCHES "^([A-Za-z_]+)[ \t]+(.+)$")
    string(TOUPPER "${CMAKE_MATCH_1}" _wlonvif_key)
    string(STRIP "${CMAKE_MATCH_2}" _wlonvif_value)
    set(WLONVIF_PROJECT_${_wlonvif_key} "${_wlonvif_value}")
  endif()
endforeach()
unset(_wlonvif_project_lines)
unset(_wlonvif_line)
unset(_wlonvif_key)
unset(_wlonvif_value)

if(NOT WLONVIF_PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR
    "PROJECT.txt version must be MAJOR.MINOR.PATCH, got '${WLONVIF_PROJECT_VERSION}'")
endif()
set(WLONVIF_PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(WLONVIF_PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
set(WLONVIF_PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
