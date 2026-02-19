cmake_minimum_required(VERSION 3.14)
project(fccu VERSION 1.0.0 LANGUAGES CXX)

# --- Core: header-only interface library ---
add_library(fccu INTERFACE)
target_include_directories(fccu INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(fccu INTERFACE cxx_std_17)

# --- Options ---
option(FCCU_BUILD_TESTS "Build tests" ON)
option(FCCU_BUILD_EXAMPLES "Build examples" ON)

# GitHub mirror prefix (set to "https://ghfast.top/" for China mainland)
set(FCCU_GITHUB_MIRROR "" CACHE STRING "GitHub mirror URL prefix")

# --- FetchContent Dependencies ---
include(FetchContent)

# Disable sub-project tests/examples
set(RINGBUFFER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RINGBUFFER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HSM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HSM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZTASK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZTASK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MCCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MCCC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Core dependencies (required)
FetchContent_Declare(ringbuffer
    GIT_REPOSITORY ${FCCU_GITHUB_MIRROR}https://github.com/DeguiLiu/ringbuffer.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)

FetchContent_Declare(hsm
    GIT_REPOSITORY https://gitee.com/liudegui/hsm-cpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)

# Optional dependencies (for examples)
FetchContent_Declare(ztask
    GIT_REPOSITORY ${FCCU_GITHUB_MIRROR}https://github.com/DeguiLiu/ztask-cpp.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE)

FetchContent_Declare(mccc
    GIT_REPOSITORY ${FCCU_GITHUB_MIRROR}https://github.com/DeguiLiu/mccc.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(ringbuffer hsm ztask mccc)

# fccu depends on ringbuffer + hsm
target_link_libraries(fccu INTERFACE ringbuffer hsm)

# --- Examples ---
if(FCCU_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Tests ---
if(FCCU_BUILD_TESTS)
    # Catch2 v3
    FetchContent_Declare(Catch2
        GIT_REPOSITORY ${FCCU_GITHUB_MIRROR}https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    add_subdirectory(tests)
endif()

# --- Install ---
include(CMakePackageConfigHelpers)
install(TARGETS fccu EXPORT fccuTargets)
install(DIRECTORY include/fccu DESTINATION include)
install(EXPORT fccuTargets
    FILE fccuConfig.cmake
    NAMESPACE fccu::
    DESTINATION lib/cmake/fccu)
