add_executable(wlonvif-media main.cpp)
target_link_libraries(wlonvif-media PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-media)
