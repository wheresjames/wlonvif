cmake_minimum_required(VERSION 3.24)

# The expected version comes from the same PROJECT.txt the library was built from.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/WlonvifVersion.cmake)
project(wlonvif_consumer VERSION ${WLONVIF_PROJECT_VERSION} LANGUAGES CXX)

find_package(wlonvif ${WLONVIF_PROJECT_VERSION} CONFIG REQUIRED COMPONENTS client)
add_executable(wlonvif_consumer main.cpp)
target_link_libraries(wlonvif_consumer PRIVATE wlonvif::client)
target_compile_definitions(wlonvif_consumer PRIVATE
  WLONVIF_EXPECTED_VERSION="${WLONVIF_PROJECT_VERSION}")
