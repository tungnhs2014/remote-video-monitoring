CMAKE_MINIMUM_REQUIRED(VERSION 3.16)

PROJECT(remote_video_monitoring LANGUAGES C CXX)

SET(CMAKE_C_STANDARD 11)
SET(CMAKE_C_STANDARD_REQUIRED ON)
SET(CMAKE_C_EXTENSIONS OFF)
SET(CMAKE_CXX_STANDARD 17)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

INCLUDE(CTest)
INCLUDE(GNUInstallDirs)

OPTION(CAMSTREAM_M1_GSTREAMER_SMOKE "Build the M1 GStreamer/GObject dependency smoke test" ON)
OPTION(CAMSTREAM_ENABLE_GRPC "Build the M6 gRPC control server and client" OFF)

# Build the shared C11 logger before its backend consumers.
ADD_SUBDIRECTORY(libs/common)
ADD_SUBDIRECTORY(libs/camera-hal)
ADD_SUBDIRECTORY(backends/camera/simulated)
ADD_SUBDIRECTORY(backends/camera/v4l2)
ADD_SUBDIRECTORY(plugins/camstreamsrc)
ADD_SUBDIRECTORY(libs/gstreamer-controller)
ADD_SUBDIRECTORY(apps/camera-service)
ADD_SUBDIRECTORY(apps/grpc-control)

IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests/smoke)
    ADD_SUBDIRECTORY(tests/camera-hal)
    ADD_SUBDIRECTORY(tests/m5)
    ADD_SUBDIRECTORY(tests/m7)
ENDIF()
