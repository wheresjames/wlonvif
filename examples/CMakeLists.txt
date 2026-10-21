# Shared CLI parsing, credentials, endpoint discovery, and synchronous task helpers.
add_library(wlonvif_example_support STATIC
  common/command_line.cpp
  common/connection.cpp
  common/terminal.cpp
  common/text.cpp)
target_include_directories(wlonvif_example_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(wlonvif_example_support PUBLIC wlonvif::client)
wlonvif_apply_project_options(wlonvif_example_support)

if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_example_support_tests common/example_support_tests.cpp)
  target_link_libraries(wlonvif_example_support_tests PRIVATE wlonvif_example_support)
  wlonvif_apply_project_options(wlonvif_example_support_tests)
  add_test(NAME example_support COMMAND wlonvif_example_support_tests)
endif()

# Shared HTTP/WebSocket server and FFmpeg video bridge for the browser examples.
if(WLONVIF_BUILD_PTZ_WEB OR WLONVIF_BUILD_EXPLORER)
  include(common/web_assets.cmake)
  add_library(wlonvif_example_web STATIC common/web/web_server.cpp common/web/video_source.cpp)
  target_include_directories(wlonvif_example_web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common/web)
  target_link_libraries(wlonvif_example_web PUBLIC wlonvif::client Boost::headers
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
  target_compile_definitions(wlonvif_example_web PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
  wlonvif_apply_project_options(wlonvif_example_web)
endif()

# Dependency-free examples are always included when the examples tree is enabled.
add_subdirectory(discover)
add_subdirectory(device-info)
add_subdirectory(device-config)
add_subdirectory(analytics)
add_subdirectory(imaging)
add_subdirectory(call)
add_subdirectory(inventory)
add_subdirectory(endpoint-policy)
add_subdirectory(virtual-device)
add_subdirectory(media)
add_subdirectory(ptz)
if(WLONVIF_BUILD_PTZ_CONTROL)
  add_subdirectory(ptz-control)
endif()
if(WLONVIF_BUILD_PTZ_WEB)
  add_subdirectory(ptz-web)
endif()
if(WLONVIF_BUILD_EXPLORER)
  add_subdirectory(explorer)
endif()
add_subdirectory(event-monitor)
add_subdirectory(camera-health)
add_subdirectory(multi-camera)

# Every example must remain safe and dependency-complete enough to print help.
if(WLONVIF_BUILD_TESTING)
  foreach(example IN ITEMS
      wlonvif-discover
      wlonvif-device-info
      wlonvif-device-config
      wlonvif-analytics
      wlonvif-imaging
      wlonvif-call
      wlonvif-inventory
      wlonvif-endpoint-policy
      wlonvif-virtual-device
      wlonvif-media
      wlonvif-ptz
      wlonvif-event-monitor
      wlonvif-camera-health
      wlonvif-multi-camera)
    add_test(NAME ${example}_help COMMAND ${example} --help)
  endforeach()
  if(WLONVIF_BUILD_PTZ_CONTROL)
    add_test(NAME wlonvif-ptz-control_help COMMAND wlonvif-ptz-control --help)
  endif()
  if(WLONVIF_BUILD_PTZ_WEB)
    add_test(NAME wlonvif-ptz-web_help COMMAND wlonvif-ptz-web --help)
  endif()
endif()
