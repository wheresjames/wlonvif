# The UI is plain HTML/CSS/JS under web/, embedded into the executable at build
# time; every file there is served at its path, so adding a module needs no C++ change.
# Everything but main(), so the tests can drive the same server.
add_library(wlonvif_explorer_core STATIC
  api_support.cpp
  cameras.cpp
  compatibility_check.cpp
  device_api.cpp
  encode.cpp
  events_api.cpp
  media_api.cpp
  ptz_api.cpp
  recorder.cpp
  router.cpp
  video_api.cpp)
wlonvif_embed_web_assets(wlonvif_explorer_core wlonvif::examples::explorer::assets
  ${CMAKE_CURRENT_SOURCE_DIR}/web)
target_include_directories(wlonvif_explorer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wlonvif_explorer_core PUBLIC wlonvif_example_web wlonvif_example_support Boost::json)
wlonvif_apply_project_options(wlonvif_explorer_core)

add_executable(wlonvif-explorer main.cpp demo.cpp)
target_link_libraries(wlonvif-explorer PRIVATE wlonvif_explorer_core wlonvif::device)
wlonvif_apply_project_options(wlonvif-explorer)
install(TARGETS wlonvif-explorer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_explorer_tests explorer_tests.cpp)
  target_link_libraries(wlonvif_explorer_tests PRIVATE wlonvif_explorer_core wlonvif::device)
  wlonvif_apply_project_options(wlonvif_explorer_tests)
  add_test(NAME explorer COMMAND wlonvif_explorer_tests)
  set_tests_properties(explorer PROPERTIES TIMEOUT 60)
  add_test(NAME wlonvif-explorer_help COMMAND wlonvif-explorer --help)
endif()
