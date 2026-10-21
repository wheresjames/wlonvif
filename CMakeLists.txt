cmake_minimum_required(VERSION 3.24)
# Name, version, description, and homepage all come from PROJECT.txt.
include(cmake/WlonvifVersion.cmake)
project(wlonvif
  VERSION ${WLONVIF_PROJECT_VERSION}
  DESCRIPTION "${WLONVIF_PROJECT_DESCRIPTION}"
  HOMEPAGE_URL "${WLONVIF_PROJECT_URL}"
  LANGUAGES CXX)

# Keep this file as the readable build map. Implementation details live in
# purpose-specific modules; dependency discovery lives under cmake/deps.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

include(cmake/WlonvifOptions.cmake)
include(cmake/WlonvifProjectOptions.cmake)
include(cmake/WlonvifDependencies.cmake)
include(cmake/WlonvifLibraries.cmake)
include(cmake/WlonvifExamples.cmake)
include(cmake/WlonvifTesting.cmake)
include(cmake/WlonvifDocs.cmake)
include(cmake/WlonvifPackaging.cmake)
