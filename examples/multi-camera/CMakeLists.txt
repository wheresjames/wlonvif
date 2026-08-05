add_executable(wlonvif-multi-camera main.cpp)
target_link_libraries(wlonvif-multi-camera PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-multi-camera)
