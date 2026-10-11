cmake_minimum_required(VERSION 3.22.1)
project(noise_android C)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

set(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../")
add_subdirectory("${REPOSITORY_ROOT}/third_party/SDL" SDL2)

add_library(main SHARED "${REPOSITORY_ROOT}/noise.c")
target_link_libraries(main PRIVATE SDL2main SDL2)
