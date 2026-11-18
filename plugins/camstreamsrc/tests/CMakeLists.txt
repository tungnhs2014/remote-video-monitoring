add_executable(camstream-src-session-test session_test.c)
target_compile_features(camstream-src-session-test PRIVATE c_std_11)
target_compile_options(camstream-src-session-test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(camstream-src-session-test PRIVATE CAMSTREAM_M4_TESTING)
target_link_libraries(camstream-src-session-test PRIVATE gstcamstreamsrc PkgConfig::CAMSTREAM_GST camstream::camera-hal)
add_dependencies(camstream-src-session-test camstream-camera-backend-simulated camstream-camera-backend-faults)

add_test(NAME camstream-src-session
    COMMAND camstream-src-session-test
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:camstream-camera-backend-faults>
        $<TARGET_FILE:gstcamstreamsrc>)

if(TARGET camstream-m4-session-smoke)
    add_test(NAME camstream-m4-session-smoke-simulated
        COMMAND camstream-m4-session-smoke
            $<TARGET_FILE:camstream-camera-backend-simulated>
            $<TARGET_FILE:gstcamstreamsrc>
            simulated0 64 48 30 1 3 2)
endif()
