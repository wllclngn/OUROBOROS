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

add_executable(test_cache unit/test_cache.cpp ${CMAKE_SOURCE_DIR}/src/backend/CacheValidation.cpp)
target_compile_features(test_cache PRIVATE cxx_std_23)
add_test(NAME cache COMMAND test_cache)

add_executable(test_queue unit/test_queue.cpp ${CMAKE_SOURCE_DIR}/src/model/QueueOps.cpp)
target_compile_features(test_queue PRIVATE cxx_std_23)
add_test(NAME queue COMMAND test_queue)

add_executable(test_genre unit/test_genre.cpp ${CMAKE_SOURCE_DIR}/src/backend/GenreDecode.cpp)
target_compile_features(test_genre PRIVATE cxx_std_23)
add_test(NAME genre COMMAND test_genre)

find_package(Threads REQUIRED)
add_executable(test_concurrency unit/test_concurrency.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/SnapshotBuffers.cpp
    ${CMAKE_SOURCE_DIR}/src/model/Snapshot.cpp)
target_compile_features(test_concurrency PRIVATE cxx_std_23)
target_link_libraries(test_concurrency Threads::Threads)
add_test(NAME concurrency COMMAND test_concurrency)

# Sort parity + bench: composite-key sublimation sort vs the old ICU comparator
# under parallel_timsort. Links sublimation, the dispatch adapter and ICU.
add_executable(test_sort unit/test_sort.cpp
    ${CMAKE_SOURCE_DIR}/src/util/SortDispatch.cpp)
target_compile_features(test_sort PRIVATE cxx_std_23)
# Optimize this target even in Debug so the bench is fair: sublimation is built
# -O2, so the TimSort template and harness must be too (not the Debug -O0 default).
target_compile_options(test_sort PRIVATE -O2 -march=native)
target_include_directories(test_sort PRIVATE ${ICU_UC_INCLUDE_DIRS} ${ICU_I18N_INCLUDE_DIRS})
target_link_libraries(test_sort sublimation ${ICU_UC_LIBRARIES} ${ICU_I18N_LIBRARIES} Threads::Threads)
add_test(NAME sort COMMAND test_sort)
