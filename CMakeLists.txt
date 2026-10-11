cmake_minimum_required(VERSION 3.20)
project(noise C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(SDL2 CONFIG REQUIRED)

add_executable(noise noise.c)
target_link_libraries(noise PRIVATE SDL2::SDL2main SDL2::SDL2)

if(WIN32)
    target_link_libraries(noise PRIVATE bcrypt)
endif()
