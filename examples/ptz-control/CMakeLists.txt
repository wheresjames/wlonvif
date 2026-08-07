# Keep deterministic input shaping independent from SDL hardware/event handling.
add_library(wlonvif_ptz_input_model STATIC input_model.cpp)
target_include_directories(wlonvif_ptz_input_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wlonvif_ptz_input_model PUBLIC wlonvif::client)
wlonvif_apply_project_options(wlonvif_ptz_input_model)

add_executable(wlonvif-ptz-control main.cpp sdl_input.cpp)
target_link_libraries(wlonvif-ptz-control PRIVATE
  wlonvif_example_support wlonvif_ptz_input_model ${WLONVIF_SDL3_TARGET})
wlonvif_apply_project_options(wlonvif-ptz-control)
install(TARGETS wlonvif-ptz-control RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_ptz_input_tests input_model_tests.cpp)
  target_link_libraries(wlonvif_ptz_input_tests PRIVATE wlonvif_ptz_input_model)
  wlonvif_apply_project_options(wlonvif_ptz_input_tests)
  add_test(NAME ptz_native_input COMMAND wlonvif_ptz_input_tests)
endif()
