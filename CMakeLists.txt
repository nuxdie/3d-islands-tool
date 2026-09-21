cmake_minimum_required(VERSION 3.20)

project(noise_islands LANGUAGES CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

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
option(ISLANDS_BUILD_PARTICLE_EXPERIMENT "Build the retired close-up PhysX experiment and tests" OFF)
if(ISLANDS_BUILD_PARTICLE_EXPERIMENT)
    include(cmake/PhysX.cmake)
    find_package(OpenGL REQUIRED)
    add_library(particle_fluid src/particle_fluid.cpp)
    target_compile_features(particle_fluid PUBLIC cxx_std_20)
    target_link_libraries(particle_fluid PUBLIC raylib OpenGL::GL)
    target_include_directories(particle_fluid SYSTEM PRIVATE "${raylib_SOURCE_DIR}/src")
    islands_link_physx(particle_fluid)
endif()

add_executable(noise_islands src/main.cpp)
target_compile_features(noise_islands PRIVATE cxx_std_20)
target_link_libraries(noise_islands PRIVATE raylib)
target_include_directories(noise_islands SYSTEM PRIVATE "${raylib_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(noise_islands PRIVATE /W4)
else()
    target_compile_options(noise_islands PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(geometry_tests tests/geometry_tests.cpp)
    target_compile_features(geometry_tests PRIVATE cxx_std_20)
    target_link_libraries(geometry_tests PRIVATE raylib)
    add_test(NAME geometry_and_water COMMAND geometry_tests)
    add_executable(gpu_water_tests tests/gpu_water_tests.cpp)
    target_compile_features(gpu_water_tests PRIVATE cxx_std_20)
    target_link_libraries(gpu_water_tests PRIVATE raylib)
    add_test(NAME gpu_water COMMAND gpu_water_tests)
    set_tests_properties(gpu_water PROPERTIES SKIP_RETURN_CODE 77)
    if(ISLANDS_BUILD_PARTICLE_EXPERIMENT)
        add_executable(particle_fluid_tests tests/particle_fluid_tests.cpp)
        target_link_libraries(particle_fluid_tests PRIVATE particle_fluid)
        add_test(NAME physx_fluid COMMAND particle_fluid_tests)
        set_tests_properties(physx_fluid PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
