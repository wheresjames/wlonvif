include_guard(GLOBAL)

# Dependency-light parsing, runtime, and security primitives.
add_library(wlonvif_core
  ${PROJECT_SOURCE_DIR}/src/core.cpp
  ${PROJECT_SOURCE_DIR}/src/xml.cpp
  ${PROJECT_SOURCE_DIR}/src/security.cpp)
add_library(wlonvif::core ALIAS wlonvif_core)
target_compile_features(wlonvif_core PUBLIC cxx_std_20)
target_include_directories(wlonvif_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(wlonvif_core PUBLIC Threads::Threads PRIVATE LibXml2::LibXml2 OpenSSL::Crypto)
wlonvif_apply_project_options(wlonvif_core)
set_target_properties(wlonvif_core PROPERTIES EXPORT_NAME core)

# Network services and reusable client-side controllers build on core.
add_library(wlonvif_client
  ${PROJECT_SOURCE_DIR}/src/client.cpp
  ${PROJECT_SOURCE_DIR}/src/curl_transport.cpp
  ${PROJECT_SOURCE_DIR}/src/discovery.cpp
  ${PROJECT_SOURCE_DIR}/src/network_services.cpp
  ${PROJECT_SOURCE_DIR}/src/ptz_controller.cpp
  ${PROJECT_SOURCE_DIR}/src/ptz_viewport.cpp
  ${PROJECT_SOURCE_DIR}/src/services.cpp)
add_library(wlonvif::client ALIAS wlonvif_client)
target_compile_features(wlonvif_client PUBLIC cxx_std_20)
target_include_directories(wlonvif_client PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(wlonvif_client PUBLIC wlonvif_core Threads::Threads PRIVATE CURL::libcurl)
if(WIN32)
  target_link_libraries(wlonvif_client PRIVATE ws2_32)
endif()
wlonvif_apply_project_options(wlonvif_client)
set_target_properties(wlonvif_client PROPERTIES EXPORT_NAME client)

# The virtual device is a separate layer for tests and embedders that need it.
add_library(wlonvif_device ${PROJECT_SOURCE_DIR}/src/virtual_device.cpp)
add_library(wlonvif::device ALIAS wlonvif_device)
target_compile_features(wlonvif_device PUBLIC cxx_std_20)
target_include_directories(wlonvif_device PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(wlonvif_device PUBLIC wlonvif_client)
wlonvif_apply_project_options(wlonvif_device)
set_target_properties(wlonvif_device PROPERTIES EXPORT_NAME device)
