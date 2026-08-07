include_guard(GLOBAL)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "WLONVIF_BUILD_FUZZERS requires Clang/libFuzzer")
endif()

# Fuzzers own their sanitizer flags and are never installed.
add_executable(wlonvif_xml_fuzzer ${PROJECT_SOURCE_DIR}/tests/fuzz/xml_fuzzer.cpp)
target_link_libraries(wlonvif_xml_fuzzer PRIVATE wlonvif::core)
target_compile_options(wlonvif_xml_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(wlonvif_xml_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(wlonvif_xml_scan_fuzzer ${PROJECT_SOURCE_DIR}/tests/fuzz/xml_scan_fuzzer.cpp)
target_include_directories(wlonvif_xml_scan_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wlonvif_xml_scan_fuzzer PRIVATE wlonvif::core)
target_compile_options(wlonvif_xml_scan_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(wlonvif_xml_scan_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
