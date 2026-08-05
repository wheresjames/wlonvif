add_executable(wlonvif-discover main.cpp)
target_link_libraries(wlonvif-discover PRIVATE wlonvif_example_support)
wlonvif_apply_project_options(wlonvif-discover)

if(WIN32)
  target_link_libraries(wlonvif-discover PRIVATE ws2_32)
endif()
