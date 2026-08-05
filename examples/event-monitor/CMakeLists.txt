add_executable(wlonvif-event-monitor main.cpp)
target_link_libraries(wlonvif-event-monitor PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-event-monitor)
