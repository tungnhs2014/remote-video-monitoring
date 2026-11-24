ADD_LIBRARY(camstream_m1_c11_probe STATIC c11_probe.c)

TARGET_COMPILE_FEATURES(camstream_m1_c11_probe PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream_m1_c11_probe PRIVATE -Wall -Wextra -Wpedantic)

ADD_EXECUTABLE(camstream_m1_language_smoke cpp17_smoke.cpp)

TARGET_COMPILE_FEATURES(camstream_m1_language_smoke PRIVATE cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream_m1_language_smoke PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream_m1_language_smoke PRIVATE camstream_m1_c11_probe)
ADD_TEST(NAME m1_language_linkage COMMAND camstream_m1_language_smoke)

IF(CAMSTREAM_M1_GSTREAMER_SMOKE)
    FIND_PACKAGE(PkgConfig REQUIRED)
    PKG_CHECK_MODULES(GSTREAMER_M1 REQUIRED IMPORTED_TARGET
        gobject-2.0
        gstreamer-1.0
        gstreamer-base-1.0
        gstreamer-video-1.0
    )

    ADD_EXECUTABLE(camstream_m1_gstreamer_smoke gstreamer_smoke.c)

    TARGET_COMPILE_FEATURES(camstream_m1_gstreamer_smoke PRIVATE c_std_11)
    TARGET_COMPILE_OPTIONS(camstream_m1_gstreamer_smoke PRIVATE -Wall -Wextra -Wpedantic)
    TARGET_LINK_LIBRARIES(camstream_m1_gstreamer_smoke PRIVATE PkgConfig::GSTREAMER_M1)
    ADD_TEST(NAME m1_gstreamer_dependencies COMMAND camstream_m1_gstreamer_smoke)
ENDIF()
