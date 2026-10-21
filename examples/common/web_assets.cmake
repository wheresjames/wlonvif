# wlonvif_embed_web_assets(<target> <namespace> <web directory>)
#
# Embed every .html, .js, and .css file under <web directory> into a generated
# `assets.hpp` in <target>'s include path, as `<namespace>::files`. The files
# are regenerated when any of them changes; adding or removing a file is picked
# up at the next build too (CONFIGURE_DEPENDS re-runs the glob).
set(WLONVIF_EMBED_ASSETS_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/embed_assets.cmake)

function(wlonvif_embed_web_assets target namespace web_dir)
  file(GLOB_RECURSE assets CONFIGURE_DEPENDS RELATIVE ${web_dir}
    ${web_dir}/*.html ${web_dir}/*.js ${web_dir}/*.css)
  list(SORT assets)
  list(JOIN assets "|" asset_list)
  list(TRANSFORM assets PREPEND ${web_dir}/ OUTPUT_VARIABLE asset_files)

  set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
  add_custom_command(
    OUTPUT ${generated}/assets.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${generated}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${generated}/assets.hpp -DROOT=${web_dir}
            -DNAMESPACE=${namespace} "-DINPUTS=${asset_list}"
            -P ${WLONVIF_EMBED_ASSETS_SCRIPT}
    DEPENDS ${asset_files} ${WLONVIF_EMBED_ASSETS_SCRIPT}
    COMMENT "Embedding ${target} web UI"
    VERBATIM)
  target_sources(${target} PRIVATE ${generated}/assets.hpp)
  target_include_directories(${target} PRIVATE ${generated})
endfunction()
