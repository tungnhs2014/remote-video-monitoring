find_package(PkgConfig REQUIRED)
pkg_check_modules(CAMSTREAM_GST REQUIRED IMPORTED_TARGET
    gobject-2.0
    gstreamer-1.0
    gstreamer-base-1.0)

add_library(gstcamstreamsrc SHARED src/camstreamsrc.c)
target_compile_features(gstcamstreamsrc PRIVATE c_std_11)
target_compile_options(gstcamstreamsrc PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(gstcamstreamsrc PRIVATE PACKAGE="remote-video-monitoring")
target_include_directories(gstcamstreamsrc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gstcamstreamsrc PRIVATE PkgConfig::CAMSTREAM_GST camstream::camera-hal camstream::common-log)
set_target_properties(gstcamstreamsrc PROPERTIES OUTPUT_NAME gstcamstreamsrc)

install(TARGETS gstcamstreamsrc LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

option(CAMSTREAM_M4_SESSION_SMOKE "Build the local/target M4 session-owner diagnostic harness" OFF)
if(CAMSTREAM_M4_SESSION_SMOKE)
    add_executable(camstream-m4-session-smoke tests/m4_session_smoke.c)
    target_compile_features(camstream-m4-session-smoke PRIVATE c_std_11)
    target_compile_options(camstream-m4-session-smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(camstream-m4-session-smoke PRIVATE gstcamstreamsrc PkgConfig::CAMSTREAM_GST camstream::camera-hal)
    install(TARGETS camstream-m4-session-smoke RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    target_compile_definitions(gstcamstreamsrc PRIVATE CAMSTREAM_M4_TESTING)
    add_subdirectory(tests)
endif()
