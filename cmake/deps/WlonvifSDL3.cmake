include_guard(GLOBAL)

set(_wlonvif_fetch_sdl3 ${WLONVIF_FETCH_PTZ_DEPENDENCIES})
if(WLONVIF_BUILD_EVERYTHING AND NOT _wlonvif_fetch_sdl3)
  # Prefer an installed package, but make the one-command "everything" build
  # complete by falling back to the pinned source when SDL3 is unavailable.
  find_package(SDL3 3.4 CONFIG QUIET)
  if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    set(_wlonvif_fetch_sdl3 ON)
    message(STATUS "WLONVIF_BUILD_EVERYTHING: SDL3 not found; fetching pinned SDL 3.4.0")
  endif()
endif()

# Fetching is explicit except for the documented one-command everything fallback.
if(_wlonvif_fetch_sdl3)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.0/SDL3-3.4.0.tar.gz
    URL_HASH SHA256=082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8)
  FetchContent_MakeAvailable(SDL3)
elseif(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
  find_package(SDL3 3.4 CONFIG REQUIRED)
endif()

# Normalize system and fetched package layouts for the native example.
if(TARGET SDL3::SDL3-static)
  set(WLONVIF_SDL3_TARGET SDL3::SDL3-static)
else()
  set(WLONVIF_SDL3_TARGET SDL3::SDL3)
endif()
unset(_wlonvif_fetch_sdl3)
