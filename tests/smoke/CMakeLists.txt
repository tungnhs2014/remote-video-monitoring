add_library(camstream_m1_c11_probe STATIC c11_probe.c)
target_compile_features(camstream_m1_c11_probe PRIVATE c_std_11)
target_compile_options(camstream_m1_c11_probe PRIVATE -Wall -Wextra -Wpedantic)

add_executable(camstream_m1_language_smoke cpp17_smoke.cpp)
target_compile_features(camstream_m1_language_smoke PRIVATE cxx_std_17)
target_compile_options(camstream_m1_language_smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream_m1_language_smoke PRIVATE camstream_m1_c11_probe)
add_test(NAME m1_language_linkage COMMAND camstream_m1_language_smoke)

if(CAMSTREAM_M1_GSTREAMER_SMOKE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GSTREAMER_M1 REQUIRED IMPORTED_TARGET
        gobject-2.0
        gstreamer-1.0
        gstreamer-base-1.0
        gstreamer-video-1.0
    )

    add_executable(camstream_m1_gstreamer_smoke gstreamer_smoke.c)
    target_compile_features(camstream_m1_gstreamer_smoke PRIVATE c_std_11)
    target_compile_options(camstream_m1_gstreamer_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(camstream_m1_gstreamer_smoke PRIVATE PkgConfig::GSTREAMER_M1)
    add_test(NAME m1_gstreamer_dependencies COMMAND camstream_m1_gstreamer_smoke)
endif()
