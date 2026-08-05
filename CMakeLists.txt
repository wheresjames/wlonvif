cmake_minimum_required(VERSION 3.24)
project(wlonvif VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(WLONVIF_BUILD_EVERYTHING "Build all supported components, tests, examples, and documentation" OFF)
option(WLONVIF_BUILD_TESTING "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(WLONVIF_BUILD_EXAMPLES "Build examples" OFF)
option(WLONVIF_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(WLONVIF_BUILD_DOCS "Build Doxygen API documentation" OFF)
option(WLONVIF_INSTALL_DOCS "Install generated API documentation" OFF)
option(WLONVIF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(WLONVIF_ENABLE_HARDENING "Enable compiler and linker hardening" ON)
option(WLONVIF_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

if(WLONVIF_BUILD_EVERYTHING)
  set(WLONVIF_BUILD_TESTING ON CACHE BOOL "Build tests" FORCE)
  set(WLONVIF_BUILD_EXAMPLES ON CACHE BOOL "Build examples" FORCE)
  set(WLONVIF_BUILD_DOCS ON CACHE BOOL "Build Doxygen API documentation" FORCE)
  set(WLONVIF_INSTALL_DOCS ON CACHE BOOL "Install generated API documentation" FORCE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(WLONVIF_BUILD_FUZZERS ON CACHE BOOL "Build libFuzzer targets (Clang only)" FORCE)
    message(STATUS "WLONVIF_BUILD_EVERYTHING: enabling Clang/libFuzzer targets")
  else()
    set(WLONVIF_BUILD_FUZZERS OFF CACHE BOOL "Build libFuzzer targets (Clang only)" FORCE)
    message(STATUS
      "WLONVIF_BUILD_EVERYTHING: skipping fuzz targets; ${CMAKE_CXX_COMPILER_ID} is not Clang")
  endif()
endif()

function(wlonvif_apply_project_options target)
  target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
  if(WLONVIF_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>)
  endif()
  if(WLONVIF_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    else()
      message(FATAL_ERROR "WLONVIF_ENABLE_SANITIZERS is supported with GCC and Clang")
    endif()
  endif()
  if(WLONVIF_ENABLE_HARDENING AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -fstack-protector-strong -D_FORTIFY_SOURCE=2)
    target_link_options(${target} PRIVATE -Wl,-z,relro,-z,now)
  endif()
endfunction()

find_package(CURL 8 REQUIRED)
find_package(LibXml2 2.9 REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(wlonvif_core
  src/core.cpp
  src/xml.cpp
  src/security.cpp)
add_library(wlonvif::core ALIAS wlonvif_core)
target_compile_features(wlonvif_core PUBLIC cxx_std_20)
target_include_directories(wlonvif_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(wlonvif_core PRIVATE LibXml2::LibXml2 OpenSSL::Crypto Threads::Threads)
wlonvif_apply_project_options(wlonvif_core)
set_target_properties(wlonvif_core PROPERTIES EXPORT_NAME core)

add_library(wlonvif_client
  src/client.cpp
  src/curl_transport.cpp
  src/discovery.cpp
  src/network_services.cpp
  src/services.cpp)
add_library(wlonvif::client ALIAS wlonvif_client)
target_compile_features(wlonvif_client PUBLIC cxx_std_20)
target_include_directories(wlonvif_client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(wlonvif_client PUBLIC wlonvif_core PRIVATE CURL::libcurl Threads::Threads)
if(WIN32)
  target_link_libraries(wlonvif_client PRIVATE ws2_32)
endif()
wlonvif_apply_project_options(wlonvif_client)
set_target_properties(wlonvif_client PROPERTIES EXPORT_NAME client)

add_library(wlonvif_device src/virtual_device.cpp)
add_library(wlonvif::device ALIAS wlonvif_device)
target_compile_features(wlonvif_device PUBLIC cxx_std_20)
target_include_directories(wlonvif_device PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(wlonvif_device PUBLIC wlonvif_client)
wlonvif_apply_project_options(wlonvif_device)
set_target_properties(wlonvif_device PROPERTIES EXPORT_NAME device)

if(WLONVIF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(WLONVIF_BUILD_TESTING)
  add_executable(wlonvif_device_tests tests/device_tests.cpp)
  target_link_libraries(wlonvif_device_tests PRIVATE wlonvif::device)
  add_test(NAME device_services COMMAND wlonvif_device_tests)
  add_executable(wlonvif_service_tests tests/service_tests.cpp)
  target_link_libraries(wlonvif_service_tests PRIVATE wlonvif::device)
  add_test(NAME media_ptz_discovery_events COMMAND wlonvif_service_tests)
  add_executable(wlonvif_qualification_tests tests/qualification_tests.cpp)
  target_link_libraries(wlonvif_qualification_tests PRIVATE wlonvif::device)
  wlonvif_apply_project_options(wlonvif_device_tests)
  wlonvif_apply_project_options(wlonvif_service_tests)
  wlonvif_apply_project_options(wlonvif_qualification_tests)
  add_test(NAME qualification COMMAND wlonvif_qualification_tests)
  set_tests_properties(qualification PROPERTIES TIMEOUT 30)
endif()

if(WLONVIF_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "WLONVIF_BUILD_FUZZERS requires Clang/libFuzzer")
  endif()
  add_executable(wlonvif_xml_fuzzer tests/fuzz/xml_fuzzer.cpp)
  target_link_libraries(wlonvif_xml_fuzzer PRIVATE wlonvif::core)
  target_compile_options(wlonvif_xml_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(wlonvif_xml_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(WLONVIF_BUILD_DOCS)
  find_package(Doxygen 1.9 REQUIRED dot)
  set(WLONVIF_DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/docs")
  configure_file(docs/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
  if(WLONVIF_BUILD_EVERYTHING)
    set(WLONVIF_DOCS_ALL ALL)
  else()
    set(WLONVIF_DOCS_ALL)
  endif()
  add_custom_target(wlonvif_docs ${WLONVIF_DOCS_ALL}
    COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating wlonvif API documentation"
    VERBATIM)
  if(WLONVIF_INSTALL_DOCS)
    install(DIRECTORY ${WLONVIF_DOXYGEN_OUTPUT_DIR}/html/
      DESTINATION ${CMAKE_INSTALL_DOCDIR}/html OPTIONAL)
  endif()
endif()

install(TARGETS wlonvif_core wlonvif_client wlonvif_device EXPORT wlonvifTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT wlonvifTargets NAMESPACE wlonvif:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wlonvif)
configure_package_config_file(cmake/wlonvifConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/wlonvifConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wlonvif)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/wlonvifConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wlonvifConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/wlonvifConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wlonvif)
