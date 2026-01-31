cmake_minimum_required(VERSION 3.20)
project(ouroboros VERSION 1.10.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force full rebuild on every configure (touch timestamp file)
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")
file(WRITE "${CMAKE_BINARY_DIR}/build_timestamp.txt" "${BUILD_TIME}\n")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Debug logging option (off by default)
option(OUROBOROS_DEBUG_LOG "Enable debug logging to /tmp/ouroboros_debug.log" OFF)

# Audio dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(SPA REQUIRED libspa-0.2)
pkg_check_modules(MPG123 REQUIRED libmpg123)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(VORBISFILE REQUIRED vorbisfile)

# M4A/AAC support via FFmpeg
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

# Unicode support for search normalization
pkg_check_modules(ICU_UC REQUIRED icu-uc)
pkg_check_modules(ICU_I18N REQUIRED icu-i18n)

# Auto-download stb headers
set(STB_DIR "${CMAKE_SOURCE_DIR}/vendor/stb")
file(MAKE_DIRECTORY "${STB_DIR}")
if(NOT EXISTS "${STB_DIR}/stb_image.h")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" "${STB_DIR}/stb_image.h")
endif()
if(NOT EXISTS "${STB_DIR}/stb_image_resize2.h")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h" "${STB_DIR}/stb_image_resize2.h")
endif()

# Glob all headers for proper dependency tracking
file(GLOB_RECURSE OUROBOROS_HEADERS
    "${CMAKE_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/*.h"
)

add_executable(ouroboros
    ${OUROBOROS_HEADERS}
    src/main.cpp
    src/model/Snapshot.cpp
    src/backend/SnapshotBuffers.cpp
    src/backend/SnapshotPublisher.cpp
    src/backend/PipeWireClient.cpp
    src/backend/Library.cpp
    src/backend/MetadataCache.cpp
    src/backend/MetadataParser.cpp
    src/backend/Config.cpp
    src/collectors/LibraryCollector.cpp
    src/collectors/PlaybackCollector.cpp
    src/ui/Terminal.cpp
    src/ui/Renderer.cpp
    src/ui/Canvas.cpp
    src/ui/Component.cpp
    src/ui/FlexLayout.cpp
    src/ui/Animation.cpp
    src/ui/Formatting.cpp
    src/ui/VisualBlocks.cpp
    src/ui/ImageRenderer.cpp
    src/ui/ArtworkWindow.cpp
    src/ui/widgets/Browser.cpp
    src/ui/widgets/Queue.cpp
    src/ui/widgets/NowPlaying.cpp
    src/ui/widgets/SearchBox.cpp
    src/ui/widgets/AlbumBrowser.cpp
    src/ui/widgets/HelpOverlay.cpp
    src/events/EventBus.cpp
    src/events/Scheduler.cpp
    src/config/KeyMap.cpp
    src/config/Theme.cpp
    src/util/TimSort.cpp
    src/util/Logger.cpp
    src/util/Platform.cpp
    src/util/ArtworkHasher.cpp
    src/util/BoyerMoore.cpp
    src/util/DirectoryScanner.cpp
    src/util/ImageDecoderPool.cpp
    src/backend/ArtworkCache.cpp
    src/audio/MP3Decoder.cpp
    src/audio/FLACDecoder.cpp
    src/audio/OGGDecoder.cpp
    src/audio/M4ADecoder.cpp
    src/audio/PipeWireOutput.cpp
    src/audio/PipeWireContext.cpp
)

# Force full rebuild on configure - all sources depend on CMakeLists.txt and timestamp
file(GLOB_RECURSE ALL_CPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set_source_files_properties(${ALL_CPP_SOURCES} PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_FILE};${CMAKE_BINARY_DIR}/build_timestamp.txt")

target_include_directories(ouroboros PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SPA_INCLUDE_DIRS}
    ${MPG123_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${VORBISFILE_INCLUDE_DIRS}
    ${ICU_UC_INCLUDE_DIRS}
    ${ICU_I18N_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)
target_compile_options(ouroboros PRIVATE -Wall -Wextra -Werror)
if(OUROBOROS_DEBUG_LOG)
    target_compile_definitions(ouroboros PRIVATE OUROBOROS_ENABLE_LOGGING)
endif()
target_link_libraries(ouroboros PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${SPA_LIBRARIES}
    ${MPG123_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${VORBISFILE_LIBRARIES}
    ${ICU_UC_LIBRARIES}
    ${ICU_I18N_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ssl
    crypto
)
set_target_properties(ouroboros PROPERTIES OUTPUT_NAME ouroboros)

# Install binary and man page
install(TARGETS ouroboros RUNTIME DESTINATION bin)
install(FILES ouroboros.1 DESTINATION share/man/man1)

# Add distclean target (like montauk workflow)
add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/CMakeFiles
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/CMakeCache.txt
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build artifacts for fresh rebuild"
)


