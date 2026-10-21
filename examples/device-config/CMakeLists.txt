add_executable(wlonvif-device-config main.cpp configuration.cpp)
target_link_libraries(wlonvif-device-config PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-device-config)
