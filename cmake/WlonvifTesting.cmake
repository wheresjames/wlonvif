include_guard(GLOBAL)

# Dependency probes are disposable acceptance tests, separate from normal examples.
if(WLONVIF_BUILD_PTZ_DEPENDENCY_SPIKES)
  add_executable(wlonvif_ptz_sdl3_spike
    ${PROJECT_SOURCE_DIR}/tests/dependency_spikes/ptz_sdl3_spike.cpp)
  target_link_libraries(wlonvif_ptz_sdl3_spike PRIVATE ${WLONVIF_SDL3_TARGET})
  wlonvif_apply_project_options(wlonvif_ptz_sdl3_spike)

  add_executable(wlonvif_ptz_beast_spike
    ${PROJECT_SOURCE_DIR}/tests/dependency_spikes/ptz_beast_spike.cpp)
  target_link_libraries(wlonvif_ptz_beast_spike PRIVATE
    Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
  target_compile_definitions(wlonvif_ptz_beast_spike PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
  wlonvif_apply_project_options(wlonvif_ptz_beast_spike)

  if(WLONVIF_BUILD_TESTING)
    add_test(NAME ptz_sdl3_dependency COMMAND wlonvif_ptz_sdl3_spike)
    add_test(NAME ptz_beast_dependency COMMAND wlonvif_ptz_beast_spike)
  endif()
endif()

# Hardware-independent unit, integration, regression, and qualification tests.
if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_device_tests ${PROJECT_SOURCE_DIR}/tests/device_tests.cpp)
  target_link_libraries(wlonvif_device_tests PRIVATE wlonvif::device)
  add_test(NAME device_services COMMAND wlonvif_device_tests)

  add_executable(wlonvif_service_tests ${PROJECT_SOURCE_DIR}/tests/service_tests.cpp)
  target_link_libraries(wlonvif_service_tests PRIVATE wlonvif::device)
  add_test(NAME media_ptz_discovery_events COMMAND wlonvif_service_tests)

  add_executable(wlonvif_qualification_tests ${PROJECT_SOURCE_DIR}/tests/qualification_tests.cpp)
  target_link_libraries(wlonvif_qualification_tests PRIVATE wlonvif::device)
  add_test(NAME qualification COMMAND wlonvif_qualification_tests)
  set_tests_properties(qualification PROPERTIES TIMEOUT 30)

  add_executable(wlonvif_regression_tests ${PROJECT_SOURCE_DIR}/tests/regression_tests.cpp)
  target_link_libraries(wlonvif_regression_tests PRIVATE wlonvif::device)
  add_test(NAME regressions COMMAND wlonvif_regression_tests)
  set_tests_properties(regressions PROPERTIES TIMEOUT 60)

  add_executable(wlonvif_ptz_controller_tests ${PROJECT_SOURCE_DIR}/tests/ptz_controller_tests.cpp)
  target_link_libraries(wlonvif_ptz_controller_tests PRIVATE wlonvif::device)
  add_test(NAME ptz_controller COMMAND wlonvif_ptz_controller_tests)
  set_tests_properties(ptz_controller PROPERTIES TIMEOUT 15)

  add_executable(wlonvif_ptz_viewport_tests ${PROJECT_SOURCE_DIR}/tests/ptz_viewport_tests.cpp)
  target_link_libraries(wlonvif_ptz_viewport_tests PRIVATE wlonvif::client)
  add_test(NAME ptz_viewport COMMAND wlonvif_ptz_viewport_tests)

  add_executable(wlonvif_http_auth_tests ${PROJECT_SOURCE_DIR}/tests/http_auth_tests.cpp)
  target_link_libraries(wlonvif_http_auth_tests PRIVATE wlonvif::client)
  add_test(NAME http_authentication COMMAND wlonvif_http_auth_tests)
  set_tests_properties(http_authentication PROPERTIES TIMEOUT 60)

  foreach(target IN ITEMS
      wlonvif_device_tests
      wlonvif_service_tests
      wlonvif_qualification_tests
      wlonvif_regression_tests
      wlonvif_ptz_controller_tests
      wlonvif_ptz_viewport_tests
      wlonvif_http_auth_tests)
    wlonvif_apply_project_options(${target})
  endforeach()
endif()

# Fuzz targets share the testing switchboard but require their own Clang setup.
if(WLONVIF_BUILD_FUZZERS)
  include(${CMAKE_CURRENT_LIST_DIR}/WlonvifFuzzing.cmake)
endif()
