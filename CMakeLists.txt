cmake_minimum_required(VERSION 3.10)
project(pcm_transport VERSION 0.9.109 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(FLAC REQUIRED flac)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

add_executable(pcm_transport
    src/main.cpp
    src/app/Application.cpp
    src/backend/AlsaPcmBackend.cpp
    src/core/PlaybackEngine.cpp
    src/cue/CueParser.cpp
    src/playlist/M3uPlaylistReader.cpp
    src/decoder/FlacStreamDecoder.cpp
    src/decoder/ExternalAudioDecoder.cpp
    src/decoder/RangeLimitedDecoder.cpp
    src/decoder/GaplessChainDecoder.cpp
    src/decoder/MemoryAudioDecoder.cpp
    src/core/TransportFifo.cpp
    src/gui/GtkPlayerWindow.cpp
    src/hardware/CardProfileRegistry.cpp
    src/dsp/AlsaControlBridge.cpp
    src/dsp/ToneControlDesign.cpp
    src/util/Logger.cpp
)

target_include_directories(pcm_transport PRIVATE
    include
    ${ALSA_INCLUDE_DIRS}
    ${FLAC_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
)

target_compile_options(pcm_transport PRIVATE
    ${ALSA_CFLAGS_OTHER}
    ${FLAC_CFLAGS_OTHER}
    ${GTK3_CFLAGS_OTHER}
    -Wall -Wextra -Wpedantic
)

target_link_directories(pcm_transport PRIVATE
    ${ALSA_LIBRARY_DIRS}
    ${FLAC_LIBRARY_DIRS}
    ${GTK3_LIBRARY_DIRS}
)

target_link_libraries(pcm_transport PRIVATE
    ${ALSA_LIBRARIES}
    ${FLAC_LIBRARIES}
    ${GTK3_LIBRARIES}
    pthread
)
