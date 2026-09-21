# PhysX 5.4.2 is paired with NVIDIA's matching prebuilt CUDA solver. No CUDA
# compiler/toolkit is needed: simulation and CUDA/OpenGL interop use driver APIs.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "The packaged PhysX GPU backend currently targets Linux x86_64 with an NVIDIA GPU.")
endif()

FetchContent_Declare(physx
    GIT_REPOSITORY https://github.com/NVIDIA-Omniverse/PhysX.git
    GIT_TAG 106.1-physx-5.4.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(physx)
FetchContent_Declare(physx_gpu
    URL "https://d4i3qtqj3r0z5.cloudfront.net/PhysXGpu%405.4.2.e5f54faf-release-106.1-linux-x86_64-public.7z"
    URL_HASH SHA256=410d19488a5b826fdc2155ecc18f9871a16038c75ed62042a4e745f6a2f76137)
FetchContent_MakeAvailable(physx_gpu)

set(PHYSX_ROOT_DIR "${physx_SOURCE_DIR}/physx")
set(PUBLIC_RELEASE ON)
set(TARGET_BUILD_PLATFORM linux)
set(PX_GENERATE_STATIC_LIBRARIES ON CACHE BOOL "Static PhysX" FORCE)
set(PX_OUTPUT_LIB_DIR "${physx_BINARY_DIR}")
set(PX_OUTPUT_BIN_DIR "${physx_BINARY_DIR}")
set(PX_OUTPUT_ARCH x86)
set(PHYSX_PHYSXGPU_PATH "${physx_gpu_SOURCE_DIR}/bin")
list(APPEND CMAKE_MODULE_PATH "${PHYSX_ROOT_DIR}/source/compiler/cmake/modules"
                            "${PHYSX_ROOT_DIR}/source/compiler/cmake")
add_subdirectory("${PHYSX_ROOT_DIR}/source/compiler/cmake" "${physx_BINARY_DIR}" EXCLUDE_FROM_ALL)
# This SDK predates current GCC's additional optimizer diagnostics. Keep them
# as warnings in vendor code, without changing warning policy for our code.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_property(physx_targets DIRECTORY "${PHYSX_ROOT_DIR}/source/compiler/cmake" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS physx_targets)
        target_compile_options(${target} PRIVATE -Wno-error)
    endforeach()
endif()

add_library(islands_physx INTERFACE)
target_include_directories(islands_physx SYSTEM INTERFACE "${PHYSX_ROOT_DIR}/include")
target_link_libraries(islands_physx INTERFACE PhysXExtensions PhysX PhysXCooking PhysXCommon PhysXFoundation ${CMAKE_DL_LIBS})
target_compile_definitions(islands_physx INTERFACE PX_PHYSX_STATIC_LIB)
set(ISLANDS_PHYSX_GPU_LIBRARY "${physx_gpu_SOURCE_DIR}/bin/linux.clang/release/libPhysXGpu_64.so")

function(islands_link_physx target)
    target_link_libraries(${target} PRIVATE islands_physx)
    target_compile_definitions(${target} PRIVATE ISLANDS_PHYSX_GPU_LIBRARY="${ISLANDS_PHYSX_GPU_LIBRARY}")
endfunction()
