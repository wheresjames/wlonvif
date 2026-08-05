add_executable(wlonvif-ptz main.cpp)
target_link_libraries(wlonvif-ptz PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-ptz)
