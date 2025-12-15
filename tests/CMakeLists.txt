project(ouroboros_tests)

# Include main source directory
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/vendor)

# Find Dependencies (Same as main project)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPG123 REQUIRED libmpg123)
pkg_check_modules(SNDFILE REQUIRED sndfile)

include_directories(${MPG123_INCLUDE_DIRS} ${SNDFILE_INCLUDE_DIRS})

# Link against common objects (or source files directly)
set(UTIL_SOURCES
    ${CMAKE_SOURCE_DIR}/src/util/TimSort.cpp
    ${CMAKE_SOURCE_DIR}/src/util/BoyerMoore.cpp
    ${CMAKE_SOURCE_DIR}/src/util/ArtworkHasher.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/MetadataParser.cpp
)

add_executable(test_utils unit/test_utils.cpp ${UTIL_SOURCES})
target_link_libraries(test_utils ${MPG123_LIBRARIES} ${SNDFILE_LIBRARIES})

add_executable(test_core unit/test_core.cpp ${UTIL_SOURCES})
target_link_libraries(test_core ${MPG123_LIBRARIES} ${SNDFILE_LIBRARIES})

add_executable(test_pipeline integration/test_pipeline.cpp ${UTIL_SOURCES})
target_link_libraries(test_pipeline ${MPG123_LIBRARIES} ${SNDFILE_LIBRARIES})

# Test runner script
add_custom_target(run_tests
    COMMAND ./test_utils
    COMMAND ./test_core
    COMMAND ./test_pipeline
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all tests..."
)
