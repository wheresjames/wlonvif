include_guard(GLOBAL)

# Example targets are isolated so library consumers do not inherit their dependencies.
if(WLONVIF_BUILD_EXAMPLES)
  add_subdirectory(${PROJECT_SOURCE_DIR}/examples ${PROJECT_BINARY_DIR}/examples)
endif()
