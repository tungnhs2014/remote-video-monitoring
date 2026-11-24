FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(CAMSTREAM_M7_GST_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)

ADD_EXECUTABLE(camstream-m7-network-test network_pipeline_test.cpp)

TARGET_COMPILE_FEATURES(camstream-m7-network-test PRIVATE cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream-m7-network-test PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-m7-network-test PRIVATE
    camstream::camera-service PkgConfig::CAMSTREAM_M7_GST_APP)
ADD_DEPENDENCIES(camstream-m7-network-test camstream-camera-backend-simulated gstcamstreamsrc)

ADD_TEST(NAME camstream-m7-network-regressions
    COMMAND camstream-m7-network-test
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:gstcamstreamsrc>)
SET_TESTS_PROPERTIES(camstream-m7-network-regressions PROPERTIES TIMEOUT 45)

ADD_TEST(NAME camstream-m7-missing-encoder
    COMMAND camstream-m7-network-test
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:gstcamstreamsrc>
        --missing-encoder)
SET_TESTS_PROPERTIES(camstream-m7-missing-encoder PROPERTIES TIMEOUT 10)

IF(CAMSTREAM_ENABLE_GRPC)
    ADD_EXECUTABLE(camstream-m7-server-options-test server_options_test.cpp)

    TARGET_COMPILE_FEATURES(camstream-m7-server-options-test PRIVATE cxx_std_17)
    TARGET_COMPILE_OPTIONS(camstream-m7-server-options-test PRIVATE -Wall -Wextra -Wpedantic)

    ADD_TEST(NAME camstream-m7-server-options
        COMMAND camstream-m7-server-options-test $<TARGET_FILE:camstream-grpc-server>)
    SET_TESTS_PROPERTIES(camstream-m7-server-options PROPERTIES TIMEOUT 10)
ENDIF()
