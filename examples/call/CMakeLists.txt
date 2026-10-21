add_executable(wlonvif-call main.cpp)
target_link_libraries(wlonvif-call PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-call)
