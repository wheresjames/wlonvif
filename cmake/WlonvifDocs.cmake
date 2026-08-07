include_guard(GLOBAL)

if(WLONVIF_BUILD_DOCS)
  # Documentation is an ALL target only for the explicit everything build.
  find_package(Doxygen 1.9 REQUIRED dot)
  set(WLONVIF_DOXYGEN_OUTPUT_DIR "${PROJECT_BINARY_DIR}/docs")
  configure_file(${PROJECT_SOURCE_DIR}/docs/Doxyfile.in ${PROJECT_BINARY_DIR}/Doxyfile @ONLY)
  if(WLONVIF_BUILD_EVERYTHING)
    set(WLONVIF_DOCS_ALL ALL)
  else()
    set(WLONVIF_DOCS_ALL)
  endif()
  add_custom_target(wlonvif_docs ${WLONVIF_DOCS_ALL}
    COMMAND ${DOXYGEN_EXECUTABLE} ${PROJECT_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating wlonvif API documentation"
    VERBATIM)
  if(WLONVIF_INSTALL_DOCS)
    install(DIRECTORY ${WLONVIF_DOXYGEN_OUTPUT_DIR}/html/
      DESTINATION ${CMAKE_INSTALL_DOCDIR}/html OPTIONAL)
  endif()
endif()
