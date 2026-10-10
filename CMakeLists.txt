cmake_minimum_required(VERSION 3.25)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")

# 0.0.0 is an unreleased-development sentinel. The first public version number
# is assigned only when the corresponding production release is complete.
project(Holotoner VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HOLOTONER_BUILD_TESTS "Build deterministic core tests" ON)
option(HOLOTONER_BUILD_PLUGIN "Build Standalone, VST3 and AU development targets" ON)

add_subdirectory(core)

if(HOLOTONER_BUILD_PLUGIN)
    include(cmake/Dependencies.cmake)
    add_subdirectory(plugin)
endif()

if(HOLOTONER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
