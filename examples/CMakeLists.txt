# Shared CLI parsing, credentials, endpoint discovery, and synchronous task helpers.
add_library(wlonvif_example_support STATIC common/example_support.cpp)
target_include_directories(wlonvif_example_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(wlonvif_example_support PUBLIC wlonvif::client)
wlonvif_apply_project_options(wlonvif_example_support)

if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_example_support_tests common/example_support_tests.cpp)
  target_link_libraries(wlonvif_example_support_tests PRIVATE wlonvif_example_support)
  wlonvif_apply_project_options(wlonvif_example_support_tests)
  add_test(NAME example_support COMMAND wlonvif_example_support_tests)
endif()

# Dependency-free examples are always included when the examples tree is enabled.
add_subdirectory(discover)
add_subdirectory(device-info)
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
add_subdirectory(event-monitor)
add_subdirectory(camera-health)
add_subdirectory(multi-camera)

# Every example must remain safe and dependency-complete enough to print help.
if(WLONVIF_BUILD_TESTING)
  foreach(example IN ITEMS
      wlonvif-discover
      wlonvif-device-info
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
