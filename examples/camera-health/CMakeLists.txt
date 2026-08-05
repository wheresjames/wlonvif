add_executable(wlonvif-camera-health main.cpp)
target_link_libraries(wlonvif-camera-health PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-camera-health)
