cmake_minimum_required(VERSION 3.24)
project(wlonvif VERSION 0.1.0 LANGUAGES CXX)

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
