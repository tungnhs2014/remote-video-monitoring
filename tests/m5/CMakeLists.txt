add_executable(camstream-m5-service-test service_test.cpp)
target_compile_features(camstream-m5-service-test PRIVATE cxx_std_17)
target_compile_options(camstream-m5-service-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-m5-service-test PRIVATE camstream::camera-service ${CMAKE_DL_LIBS})
add_test(NAME camstream-m5-service-regressions
    COMMAND camstream-m5-service-test
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:camstream-camera-backend-faults>
        $<TARGET_FILE:gstcamstreamsrc>)
add_test(NAME camstream-m5-service-smoke-simulated
    COMMAND camstream-m5-service-smoke
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:gstcamstreamsrc>
        simulated0 64 48 30 1 3 2)
