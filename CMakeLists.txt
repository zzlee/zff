cmake_minimum_required(VERSION 3.16)
project(zff VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_JETSON "Enable NVIDIA Jetson NvBufSurface support" OFF)
option(ENABLE_DANTE "Enable Dante DEP protocol support" ON)

if(ENABLE_JETSON)
    add_compile_definitions(ENABLE_JETSON=1)
endif()

# Find FFmpeg dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVFILTER REQUIRED libavfilter)
pkg_check_modules(AVDEVICE REQUIRED libavdevice)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(FREETYPE freetype2)
find_library(SRT_LIBRARIES NAMES srt-gnutls srt)
find_path(SRT_INCLUDE_DIRS NAMES srt/srt.h PATHS /usr/include /usr/local/include)

# SVT-JPEG-XS (ST 2110-22 software codec, optional)
pkg_check_modules(SVT_JPEGXS SvtJpegxs)
if(SVT_JPEGXS_FOUND)
    set(HAS_SVT_JPEGXS ON)
    message(STATUS "SVT-JPEG-XS enabled (ST 2110-22 encode/decode)")
else()
    set(HAS_SVT_JPEGXS OFF)
    message(STATUS "SVT-JPEG-XS not found (ST 2110-22 codec tests skip)")
endif()

# WebRTC (libdatachannel, patched with outgoing track interceptors for TWCC)
find_package(LibDataChannel QUIET)
if(NOT LibDataChannel_FOUND)
    pkg_check_modules(DATACHANNEL QUIET libdatachannel)
endif()
if(LibDataChannel_FOUND OR DATACHANNEL_FOUND)
    set(HAS_WEBRTC ON)
    message(STATUS "WebRTC enabled (libdatachannel found)")
else()
    set(HAS_WEBRTC OFF)
    message(STATUS "WebRTC disabled (libdatachannel not found)")
endif()

# Include directories
include_directories(
    include
    ${AVUTIL_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVFILTER_INCLUDE_DIRS}
    ${AVDEVICE_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${SRT_INCLUDE_DIRS}
)

# Core SDK Library (libzff-core)
set(CORE_SOURCES
    src/core/zff_core.c
    src/core/zff_hw.c
    src/core/zff_time.c
)

add_library(zff-core ${CORE_SOURCES})

target_link_libraries(zff-core
    PUBLIC
    ${AVUTIL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVFILTER_LIBRARIES}
)

# Plugins Library (libzff-plugins)
set(PLUGINS_SOURCES
    src/plugins/zff_plugins.c
    src/plugins/video/zstr_videotestsrc.c
    src/plugins/audio/zstr_audiotestsrc.c
    src/plugins/audio/zstr_aresample.c
    src/plugins/video/zstr_scale.c
    src/plugins/devices/zstr_v4l2.c
    src/plugins/devices/zstr_alsa.c
    src/plugins/devices/zstr_v4l2_sink.c
    src/plugins/devices/zstr_alsa_sink.c
    src/plugins/audio/zstr_amix.c
    src/plugins/video/zstr_glsink.c
    src/plugins/video/zstr_text_overlay.c
    src/plugins/video/zstr_gl_comp.c
    src/plugins/streaming/zstr_net_source.c
    src/plugins/streaming/zstr_net_sink.c
    src/plugins/streaming/zstr_rtsp_server.c
    src/plugins/streaming/zstr_rtp_payloader.c
    src/plugins/streaming/zstr_rtp_depayloader.c
    src/plugins/broadcast/zstr_st2110_20.c
    src/plugins/broadcast/zstr_st2110_21.c
    src/plugins/broadcast/zstr_st2110_22.c
    src/plugins/broadcast/zstr_st2110_30.c
    src/plugins/broadcast/zstr_st2110_40.c
    src/plugins/broadcast/zstr_st2022_5.c
    src/plugins/broadcast/zstr_st2022_7.c
    src/plugins/broadcast/zstr_st2110_device.c
    src/plugins/streaming/zstr_srt_parser.c
    src/plugins/streaming/zstr_srt_common.c
    src/plugins/streaming/zstr_srt_source.c
    src/plugins/streaming/zstr_srt_sink.c
)

if(HAS_WEBRTC)
    list(APPEND PLUGINS_SOURCES
        src/plugins/streaming/zstr_webrtc.c
        src/plugins/streaming/zstr_webrtc_twcc.c
        src/plugins/streaming/zstr_webrtc_sdp.c
        src/plugins/streaming/zstr_webrtc_device.c
        src/plugins/streaming/zstr_signaling.c
        src/plugins/streaming/zstr_webrtc_signaling.c
        src/plugins/streaming/zstr_whip.c
        src/plugins/streaming/zstr_webrtc_whip.c
    )
    add_compile_definitions(HAS_WEBRTC=1)
endif()

add_library(zff-plugins ${PLUGINS_SOURCES})

if(HAS_WEBRTC)
    if(LibDataChannel_FOUND)
        get_target_property(_ldc_includes LibDataChannel::LibDataChannel INTERFACE_INCLUDE_DIRECTORIES)
        if(_ldc_includes)
            target_include_directories(zff-plugins PRIVATE ${_ldc_includes})
        endif()
        target_link_libraries(zff-plugins PUBLIC LibDataChannel::LibDataChannel)
    elseif(DATACHANNEL_FOUND)
        target_include_directories(zff-plugins PRIVATE ${DATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(zff-plugins PUBLIC ${DATACHANNEL_LIBRARIES})
    endif()
endif()

if(HAS_SVT_JPEGXS)
    add_compile_definitions(ZSTR_HAVE_SVT_JPEGXS=1)
    target_include_directories(zff-plugins PRIVATE ${SVT_JPEGXS_INCLUDE_DIRS})
    target_link_libraries(zff-plugins PUBLIC ${SVT_JPEGXS_LIBRARIES})
endif()

target_link_libraries(zff-plugins
    PUBLIC
    zff-core
    ${AVUTIL_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVFILTER_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${GL_LIBRARIES}
    ${X11_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${SRT_LIBRARIES}
    m
    pthread
)

# Testing
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_core tests/test_core.c)
    target_link_libraries(test_core PRIVATE zff-core)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_videotestsrc tests/test_videotestsrc.c)
    target_link_libraries(test_videotestsrc PRIVATE zff-plugins zff-core)
    add_test(NAME test_videotestsrc COMMAND test_videotestsrc)

    add_executable(test_audiotestsrc tests/test_audiotestsrc.c)
    target_link_libraries(test_audiotestsrc PRIVATE zff-plugins zff-core m)
    add_test(NAME test_audiotestsrc COMMAND test_audiotestsrc)

    add_executable(test_aresample tests/test_aresample.c)
    target_link_libraries(test_aresample PRIVATE zff-plugins zff-core ${SWRESAMPLE_LIBRARIES} m)
    add_test(NAME test_aresample COMMAND test_aresample)

    add_executable(test_scale tests/test_scale.c)
    target_link_libraries(test_scale PRIVATE zff-plugins zff-core ${SWSCALE_LIBRARIES} m)
    add_test(NAME test_scale COMMAND test_scale)

    add_executable(test_v4l2 tests/test_v4l2.c)
    target_link_libraries(test_v4l2 PRIVATE zff-plugins zff-core)
    add_test(NAME test_v4l2 COMMAND test_v4l2)

    add_executable(test_alsa tests/test_alsa.c)
    target_link_libraries(test_alsa PRIVATE zff-plugins zff-core m)
    add_test(NAME test_alsa COMMAND test_alsa)

    add_executable(test_v4l2_sink tests/test_v4l2_sink.c)
    target_link_libraries(test_v4l2_sink PRIVATE zff-plugins zff-core)
    add_test(NAME test_v4l2_sink COMMAND test_v4l2_sink)

    add_executable(test_alsa_sink tests/test_alsa_sink.c)
    target_link_libraries(test_alsa_sink PRIVATE zff-plugins zff-core m)
    add_test(NAME test_alsa_sink COMMAND test_alsa_sink)

    add_executable(test_amix tests/test_amix.c)
    target_link_libraries(test_amix PRIVATE zff-plugins zff-core ${SWRESAMPLE_LIBRARIES} m)
    add_test(NAME test_amix COMMAND test_amix)

    add_executable(test_glsink tests/test_glsink.c)
    target_link_libraries(test_glsink PRIVATE zff-plugins zff-core ${GL_LIBRARIES} ${X11_LIBRARIES})
    add_test(NAME test_glsink COMMAND test_glsink)

    add_executable(test_text_overlay tests/test_text_overlay.c)
    target_link_libraries(test_text_overlay PRIVATE zff-plugins zff-core ${FREETYPE_LIBRARIES} m)
    add_test(NAME test_text_overlay COMMAND test_text_overlay)

    add_executable(test_gl_comp tests/test_gl_comp.c)
    target_link_libraries(test_gl_comp PRIVATE zff-plugins zff-core ${GL_LIBRARIES} ${X11_LIBRARIES} ${SWSCALE_LIBRARIES} m)
    add_test(NAME test_gl_comp COMMAND test_gl_comp)

    add_executable(test_net tests/test_net.c)
    target_link_libraries(test_net PRIVATE zff-plugins zff-core pthread)
    add_test(NAME test_net COMMAND test_net)

    add_executable(test_rtp tests/test_rtp.c)
    target_link_libraries(test_rtp PRIVATE zff-plugins zff-core)
    add_test(NAME test_rtp COMMAND test_rtp)

    add_executable(test_rtsp_server tests/test_rtsp_server.c)
    target_link_libraries(test_rtsp_server PRIVATE zff-plugins zff-core pthread)
    add_test(NAME test_rtsp_server COMMAND test_rtsp_server)

    if(HAS_WEBRTC)
        add_executable(test_webrtc tests/test_webrtc.c)
        target_link_libraries(test_webrtc PRIVATE zff-plugins zff-core pthread)
        add_test(NAME test_webrtc COMMAND test_webrtc)

        add_executable(test_webrtc_device tests/test_webrtc_device.c)
        target_link_libraries(test_webrtc_device PRIVATE zff-plugins zff-core pthread)
        add_test(NAME test_webrtc_device COMMAND test_webrtc_device)

        add_executable(test_webrtc_sdp tests/test_webrtc_sdp.c)
        target_link_libraries(test_webrtc_sdp PRIVATE zff-plugins zff-core)
        add_test(NAME test_webrtc_sdp COMMAND test_webrtc_sdp)

        add_executable(test_webrtc_signaling tests/test_webrtc_signaling.c)
        target_link_libraries(test_webrtc_signaling PRIVATE zff-plugins zff-core pthread)
        add_test(NAME test_webrtc_signaling COMMAND test_webrtc_signaling)

        add_executable(test_webrtc_whip tests/test_webrtc_whip.c)
        target_link_libraries(test_webrtc_whip PRIVATE zff-plugins zff-core pthread)
        add_test(NAME test_webrtc_whip COMMAND test_webrtc_whip)

        add_executable(test_webrtc_page tests/test_webrtc_page.c)
        target_link_libraries(test_webrtc_page PRIVATE zff-plugins zff-core pthread)
        add_test(NAME test_webrtc_page COMMAND test_webrtc_page)
    endif()

    add_executable(test_st2110 tests/test_st2110.c)
    target_link_libraries(test_st2110 PRIVATE zff-plugins zff-core pthread)
    add_test(NAME test_st2110 COMMAND test_st2110)

    add_executable(test_srt tests/test_srt.c)
    target_link_libraries(test_srt PRIVATE zff-plugins zff-core ${SRT_LIBRARIES} pthread)
    add_test(NAME test_srt COMMAND test_srt)
endif()

message(STATUS "zff configured successfully.")

# Examples (manual demos, not part of ctest)
add_subdirectory(examples)
