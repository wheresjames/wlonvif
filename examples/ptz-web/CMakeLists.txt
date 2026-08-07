# Protocol and calibration logic stay transport-independent for deterministic tests/fuzzing.
add_library(wlonvif_ptz_web_protocol STATIC protocol.cpp calibration.cpp)
target_include_directories(wlonvif_ptz_web_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wlonvif_ptz_web_protocol PUBLIC wlonvif::client PRIVATE Boost::json)
wlonvif_apply_project_options(wlonvif_ptz_web_protocol)

add_executable(wlonvif-ptz-web main.cpp server.cpp ffmpeg_video.cpp)
target_link_libraries(wlonvif-ptz-web PRIVATE wlonvif_example_support
  wlonvif_ptz_web_protocol Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_definitions(wlonvif-ptz-web PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
wlonvif_apply_project_options(wlonvif-ptz-web)
install(TARGETS wlonvif-ptz-web RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Server tests use a loopback socket; the FFmpeg fake avoids a runtime media dependency.
if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_ptz_web_protocol_tests protocol_tests.cpp)
  target_link_libraries(wlonvif_ptz_web_protocol_tests PRIVATE wlonvif_ptz_web_protocol)
  wlonvif_apply_project_options(wlonvif_ptz_web_protocol_tests)
  add_test(NAME ptz_web_protocol COMMAND wlonvif_ptz_web_protocol_tests)
  add_executable(wlonvif_ptz_web_server_tests server_tests.cpp server.cpp)
  target_link_libraries(wlonvif_ptz_web_server_tests PRIVATE wlonvif_ptz_web_protocol
    Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
  target_compile_definitions(wlonvif_ptz_web_server_tests PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
  wlonvif_apply_project_options(wlonvif_ptz_web_server_tests)
  add_test(NAME ptz_web_server COMMAND wlonvif_ptz_web_server_tests)
  set_tests_properties(ptz_web_server PROPERTIES TIMEOUT 10)
  add_executable(wlonvif_ptz_web_ffmpeg_fake ffmpeg_fake.cpp)
  add_executable(wlonvif_ptz_web_ffmpeg_tests ffmpeg_video_tests.cpp ffmpeg_video.cpp)
  target_link_libraries(wlonvif_ptz_web_ffmpeg_tests PRIVATE wlonvif_ptz_web_protocol Threads::Threads)
  wlonvif_apply_project_options(wlonvif_ptz_web_ffmpeg_tests)
  add_test(NAME ptz_web_ffmpeg COMMAND wlonvif_ptz_web_ffmpeg_tests
           $<TARGET_FILE:wlonvif_ptz_web_ffmpeg_fake>)
  set_tests_properties(ptz_web_ffmpeg PROPERTIES TIMEOUT 5)
endif()

# Fuzz only the bounded, authenticated JSON protocol boundary.
if(WLONVIF_BUILD_FUZZERS)
  add_executable(wlonvif_ptz_web_protocol_fuzzer protocol_fuzzer.cpp)
  target_link_libraries(wlonvif_ptz_web_protocol_fuzzer PRIVATE wlonvif_ptz_web_protocol)
  target_compile_options(wlonvif_ptz_web_protocol_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(wlonvif_ptz_web_protocol_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
