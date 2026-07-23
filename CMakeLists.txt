cmake_minimum_required(VERSION 3.10)
project(pcm_transport VERSION 0.9.110 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(FLAC REQUIRED flac)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
find_package(OpenSSL REQUIRED)

find_program(GLIB_COMPILE_RESOURCES_EXECUTABLE
    NAMES glib-compile-resources
)
if(NOT GLIB_COMPILE_RESOURCES_EXECUTABLE)
    message(FATAL_ERROR "glib-compile-resources was not found")
endif()

set(PCM_TRANSPORT_RESOURCE_XML
    ${CMAKE_CURRENT_SOURCE_DIR}/data/org.berestov.pcmtransport.gresource.xml
)
set(PCM_TRANSPORT_RESOURCE_SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/pcm_transport_resources.c
)
set(PCM_TRANSPORT_EMBEDDED_ICONS
    ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/16x16/apps/org.berestov.pcmtransport.png
    ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/32x32/apps/org.berestov.pcmtransport.png
    ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/48x48/apps/org.berestov.pcmtransport.png
    ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/128x128/apps/org.berestov.pcmtransport.png
)

add_custom_command(
    OUTPUT ${PCM_TRANSPORT_RESOURCE_SOURCE}
    COMMAND ${GLIB_COMPILE_RESOURCES_EXECUTABLE}
            --generate-source
            --target=${PCM_TRANSPORT_RESOURCE_SOURCE}
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor
            ${PCM_TRANSPORT_RESOURCE_XML}
    DEPENDS
        ${PCM_TRANSPORT_RESOURCE_XML}
        ${PCM_TRANSPORT_EMBEDDED_ICONS}
    VERBATIM
)

set_source_files_properties(${PCM_TRANSPORT_RESOURCE_SOURCE} PROPERTIES GENERATED TRUE)

add_executable(pcm_transport
    src/main.cpp
    src/app/Application.cpp
    src/backend/AlsaPcmBackend.cpp
    src/core/PlaybackEngine.cpp
    src/cue/CueParser.cpp
    src/util/MediaUri.cpp
    src/stream/IcyMetadataClient.cpp
    src/stream/StreamSidecar.cpp
    src/stream/StreamHealthRegistry.cpp
    src/playlist/M3uPlaylistReader.cpp
    src/playlist/MediaProbe.cpp
    src/decoder/FlacStreamDecoder.cpp
    src/decoder/ExternalAudioDecoder.cpp
    src/decoder/RangeLimitedDecoder.cpp
    src/decoder/GaplessChainDecoder.cpp
    src/decoder/MemoryAudioDecoder.cpp
    src/core/TransportFifo.cpp
    src/gui/GtkPlayerWindow.cpp
    src/patches/PlaylistSessionController.cpp
    src/patches/StreamAudioDecoder.cpp
    src/patches/StreamPlaybackManager.cpp
    src/patches/StreamPlaylistUtils.cpp
    src/patches/M3uPlaylistExtensions.cpp
    src/session/PlaylistSession.cpp
    src/mpris/MprisService.cpp
    src/hardware/CardProfileRegistry.cpp
    src/dsp/AlsaControlBridge.cpp
    src/dsp/ToneControlDesign.cpp
    src/util/Logger.cpp
    src/util/ManagedSubprocess.cpp
    src/util/TextEncoding.cpp
    ${PCM_TRANSPORT_RESOURCE_SOURCE}
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
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

install(TARGETS pcm_transport
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES data/org.berestov.pcmtransport.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

foreach(ICON_SIZE 16 32 48 128 256)
    install(
        FILES data/icons/hicolor/${ICON_SIZE}x${ICON_SIZE}/apps/org.berestov.pcmtransport.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${ICON_SIZE}x${ICON_SIZE}/apps
    )
endforeach()
