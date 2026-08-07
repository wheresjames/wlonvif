include_guard(GLOBAL)

# Public build switches are centralized here so presets and parent projects see one surface.
option(WLONVIF_BUILD_EVERYTHING "Build all supported components, tests, examples, and documentation" OFF)
option(WLONVIF_BUILD_TESTING "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(WLONVIF_BUILD_EXAMPLES "Build examples" OFF)
option(WLONVIF_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(WLONVIF_BUILD_DOCS "Build Doxygen API documentation" OFF)
option(WLONVIF_INSTALL_DOCS "Install generated API documentation" OFF)
option(WLONVIF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(WLONVIF_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(WLONVIF_ENABLE_HARDENING "Enable compiler and linker hardening" ON)
option(WLONVIF_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(WLONVIF_BUILD_PTZ_DEPENDENCY_SPIKES "Build optional SDL3 and Beast PTZ acceptance probes" OFF)
option(WLONVIF_BUILD_PTZ_CONTROL "Build the optional SDL3 native PTZ controller example" OFF)
option(WLONVIF_BUILD_PTZ_WEB "Build the optional secure browser PTZ controller example" OFF)
option(WLONVIF_FETCH_PTZ_DEPENDENCIES "Fetch pinned optional PTZ example dependencies" OFF)

# The convenience build is deterministic: unsupported compiler-specific targets are skipped.
if(WLONVIF_BUILD_EVERYTHING)
  set(WLONVIF_BUILD_TESTING ON CACHE BOOL "Build tests" FORCE)
  set(WLONVIF_BUILD_EXAMPLES ON CACHE BOOL "Build examples" FORCE)
  set(WLONVIF_BUILD_PTZ_CONTROL ON CACHE BOOL
    "Build the optional SDL3 native PTZ controller example" FORCE)
  set(WLONVIF_BUILD_PTZ_WEB ON CACHE BOOL
    "Build the optional secure browser PTZ controller example" FORCE)
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
