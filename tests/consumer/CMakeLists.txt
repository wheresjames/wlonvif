cmake_minimum_required(VERSION 3.24)
project(wlonvif_consumer LANGUAGES CXX)

find_package(wlonvif 0.1 CONFIG REQUIRED COMPONENTS client)
add_executable(wlonvif_consumer main.cpp)
target_link_libraries(wlonvif_consumer PRIVATE wlonvif::client)
