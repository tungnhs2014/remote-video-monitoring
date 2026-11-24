ADD_EXECUTABLE(camstream-src-session-test session_test.c)

TARGET_COMPILE_FEATURES(camstream-src-session-test PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-src-session-test PRIVATE -Wall -Wextra -Wpedantic)
TARGET_COMPILE_DEFINITIONS(camstream-src-session-test PRIVATE CAMSTREAM_M4_TESTING)
TARGET_LINK_LIBRARIES(camstream-src-session-test PRIVATE
    gstcamstreamsrc PkgConfig::CAMSTREAM_GST camstream::camera-hal)
ADD_DEPENDENCIES(camstream-src-session-test
    camstream-camera-backend-simulated camstream-camera-backend-faults)

ADD_TEST(NAME camstream-src-session
    COMMAND camstream-src-session-test
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:camstream-camera-backend-faults>
        $<TARGET_FILE:gstcamstreamsrc>)

IF(TARGET camstream-m4-session-smoke)
    ADD_TEST(NAME camstream-m4-session-smoke-simulated
        COMMAND camstream-m4-session-smoke
            $<TARGET_FILE:camstream-camera-backend-simulated>
            $<TARGET_FILE:gstcamstreamsrc>
            simulated0 64 48 30 1 3 2)
ENDIF()
