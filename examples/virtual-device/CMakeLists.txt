add_executable(wlonvif-virtual-device main.cpp)
target_link_libraries(wlonvif-virtual-device PRIVATE wlonvif_example_support wlonvif::device)
wlonvif_apply_project_options(wlonvif-virtual-device)
