FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(CAMSTREAM_GST REQUIRED IMPORTED_TARGET GLOBAL
    gobject-2.0
    gstreamer-1.0
    gstreamer-base-1.0)

ADD_LIBRARY(gstcamstreamsrc SHARED src/camstreamsrc.c)

TARGET_COMPILE_FEATURES(gstcamstreamsrc PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(gstcamstreamsrc PRIVATE -Wall -Wextra -Wpedantic)
TARGET_COMPILE_DEFINITIONS(gstcamstreamsrc PRIVATE PACKAGE="remote-video-monitoring")
TARGET_INCLUDE_DIRECTORIES(gstcamstreamsrc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
TARGET_LINK_LIBRARIES(gstcamstreamsrc PRIVATE
    PkgConfig::CAMSTREAM_GST camstream::camera-hal camstream::common-log)
SET_TARGET_PROPERTIES(gstcamstreamsrc PROPERTIES OUTPUT_NAME gstcamstreamsrc)

INSTALL(TARGETS gstcamstreamsrc LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0 COMPONENT Runtime)
INSTALL(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

OPTION(CAMSTREAM_M4_SESSION_SMOKE "Build the local/target M4 session-owner diagnostic harness" OFF)
IF(CAMSTREAM_M4_SESSION_SMOKE)
    ADD_EXECUTABLE(camstream-m4-session-smoke tests/m4_session_smoke.c)

    TARGET_COMPILE_FEATURES(camstream-m4-session-smoke PRIVATE c_std_11)
    TARGET_COMPILE_OPTIONS(camstream-m4-session-smoke PRIVATE -Wall -Wextra -Wpedantic)
    TARGET_LINK_LIBRARIES(camstream-m4-session-smoke PRIVATE
        gstcamstreamsrc PkgConfig::CAMSTREAM_GST camstream::camera-hal)

    INSTALL(TARGETS camstream-m4-session-smoke RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
ENDIF()

IF(BUILD_TESTING)
    TARGET_COMPILE_DEFINITIONS(gstcamstreamsrc PRIVATE CAMSTREAM_M4_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
