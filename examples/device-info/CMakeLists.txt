add_executable(wlonvif-device-info main.cpp)
target_link_libraries(wlonvif-device-info PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-device-info)
