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
    src/plugins/video/zstr_videotestsrc.c
    src/plugins/audio/zstr_audiotestsrc.c
)

add_library(zff-plugins ${PLUGINS_SOURCES})

target_link_libraries(zff-plugins
    PUBLIC
    zff-core
    ${AVUTIL_LIBRARIES}
    ${AVFILTER_LIBRARIES}
    m
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
endif()

message(STATUS "zff configured successfully.")
