# Test suites. Pulled in via add_subdirectory(tests) from the root CMakeLists;
# CMAKE_SOURCE_DIR therefore refers to the repository root.
# Framework: framework/SimpleTest.hpp (zero dependencies).

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/vendor)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Test targets register here as suites land. Pattern:
#   add_executable(test_<area> unit/test_<area>.cpp <sources under test>)
#   add_test(NAME <area> COMMAND test_<area>)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/framework)

add_executable(test_fft unit/test_fft.cpp ${CMAKE_SOURCE_DIR}/src/util/Fft.cpp)
target_compile_features(test_fft PRIVATE cxx_std_23)
add_test(NAME fft COMMAND test_fft)
