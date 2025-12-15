cmake_minimum_required(VERSION 3.20)
project(ouroboros LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Audio dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(SPA REQUIRED libspa-0.2)
pkg_check_modules(MPG123 REQUIRED libmpg123)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(VORBISFILE REQUIRED vorbisfile)

# Auto-download stb headers
set(STB_DIR "${CMAKE_SOURCE_DIR}/vendor/stb")
file(MAKE_DIRECTORY "${STB_DIR}")
if(NOT EXISTS "${STB_DIR}/stb_image.h")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" "${STB_DIR}/stb_image.h")
endif()
if(NOT EXISTS "${STB_DIR}/stb_image_resize2.h")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h" "${STB_DIR}/stb_image_resize2.h")
endif()

add_executable(ouroboros
    src/main.cpp
    src/model/Snapshot.cpp
    src/backend/SnapshotBuffers.cpp
    src/backend/SnapshotPublisher.cpp
    src/backend/PipeWireClient.cpp
    src/backend/Library.cpp
    src/backend/Queue.cpp
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
    src/ui/ArtworkLoader.cpp
    src/ui/widgets/Browser.cpp
    src/ui/widgets/Queue.cpp
    src/ui/widgets/NowPlaying.cpp
    src/ui/widgets/Controls.cpp
    src/ui/widgets/StatusBar.cpp
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
    src/audio/PipeWireOutput.cpp
    src/audio/PipeWireContext.cpp
)
target_include_directories(ouroboros PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SPA_INCLUDE_DIRS}
    ${MPG123_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${VORBISFILE_INCLUDE_DIRS}
)
target_compile_options(ouroboros PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ouroboros PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${SPA_LIBRARIES}
    ${MPG123_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${VORBISFILE_LIBRARIES}
    ssl
    crypto
)
set_target_properties(ouroboros PROPERTIES OUTPUT_NAME ouroboros)

install(TARGETS ouroboros RUNTIME DESTINATION bin)

# Add distclean target (like montauk workflow)
add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/CMakeFiles
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/CMakeCache.txt
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build artifacts for fresh rebuild"
)

enable_testing()
add_subdirectory(tests)
