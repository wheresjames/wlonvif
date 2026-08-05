add_executable(wlonvif-endpoint-policy main.cpp)
target_link_libraries(wlonvif-endpoint-policy PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-endpoint-policy)
