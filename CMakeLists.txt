cmake_minimum_required(VERSION 3.16)
project(zff VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_JETSON "Enable NVIDIA Jetson NvBufSurface support" OFF)
option(ENABLE_DANTE "Enable Dante DEP protocol support" ON)

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

# Core SDK Library
add_library(zff-core
    include/zff/zff_core.h
    include/zff/zff_hw.h
    include/zff/zff_time.h
)

target_link_libraries(zff-core
    PRIVATE
    ${AVUTIL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
)

message(STATUS "zff configured successfully.")
