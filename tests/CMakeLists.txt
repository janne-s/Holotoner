add_executable(holotoner-core-tests CoreTests.cpp)
target_link_libraries(holotoner-core-tests PRIVATE holotoner_core)
target_compile_features(holotoner-core-tests PRIVATE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(holotoner-core-tests PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

add_test(NAME holotoner-core COMMAND holotoner-core-tests)
