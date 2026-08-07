include_guard(GLOBAL)

# Core dependencies are always resolved; frontend-only packages remain opt-in.
include(${CMAKE_CURRENT_LIST_DIR}/deps/WlonvifCurl.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/deps/WlonvifLibXml2.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/deps/WlonvifOpenSSL.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/deps/WlonvifThreads.cmake)

# Keep SDL3 and Boost out of core-only configure and install paths.
if(WLONVIF_BUILD_PTZ_CONTROL OR WLONVIF_BUILD_PTZ_DEPENDENCY_SPIKES)
  include(${CMAKE_CURRENT_LIST_DIR}/deps/WlonvifSDL3.cmake)
endif()
if(WLONVIF_BUILD_PTZ_WEB OR WLONVIF_BUILD_PTZ_DEPENDENCY_SPIKES)
  include(${CMAKE_CURRENT_LIST_DIR}/deps/WlonvifBeast.cmake)
endif()
