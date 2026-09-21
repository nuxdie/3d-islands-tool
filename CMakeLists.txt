cmake_minimum_required(VERSION 3.20)

project(noise_islands LANGUAGES CXX)

include(FetchContent)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

add_executable(noise_islands src/main.cpp)
target_compile_features(noise_islands PRIVATE cxx_std_20)
target_link_libraries(noise_islands PRIVATE raylib)

if(MSVC)
    target_compile_options(noise_islands PRIVATE /W4)
else()
    target_compile_options(noise_islands PRIVATE -Wall -Wextra -Wpedantic)
endif()
