cmake_minimum_required(VERSION 3.16)

project(remote_video_monitoring LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

option(CAMSTREAM_M1_GSTREAMER_SMOKE "Build the M1 GStreamer/GObject dependency smoke test" ON)

# These directories reserve the approved module boundaries. M1 adds no application implementation targets.
add_subdirectory(libs/camera-hal)
add_subdirectory(backends/camera/simulated)
add_subdirectory(backends/camera/v4l2)
add_subdirectory(plugins/camstreamsrc)
add_subdirectory(libs/gstreamer-controller)
add_subdirectory(apps/camera-service)
add_subdirectory(apps/grpc-control)

if(BUILD_TESTING)
    add_subdirectory(tests/smoke)
    add_subdirectory(tests/camera-hal)
endif()
