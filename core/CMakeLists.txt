add_library(holotoner_core STATIC
    audio/SourcePlayer.cpp
    dsp/StereoWidth.cpp
    geometry/PlaneProjection.cpp)

target_include_directories(holotoner_core PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_features(holotoner_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(holotoner_core PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()
