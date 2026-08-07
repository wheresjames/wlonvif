include_guard(GLOBAL)

# Export only library targets; optional examples retain private dependencies.
install(TARGETS wlonvif_core wlonvif_client wlonvif_device EXPORT wlonvifTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT wlonvifTargets NAMESPACE wlonvif::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wlonvif)

# Generate a relocatable find_package(wlonvif) configuration and version check.
configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/wlonvifConfig.cmake.in
  ${PROJECT_BINARY_DIR}/wlonvifConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wlonvif)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/wlonvifConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
  ${PROJECT_BINARY_DIR}/wlonvifConfig.cmake
  ${PROJECT_BINARY_DIR}/wlonvifConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wlonvif)
