ADD_EXECUTABLE(camstream-m5-service-test service_test.cpp)

TARGET_COMPILE_FEATURES(camstream-m5-service-test PRIVATE cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream-m5-service-test PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-m5-service-test PRIVATE camstream::camera-service ${CMAKE_DL_LIBS})

ADD_TEST(NAME camstream-m5-service-regressions
    COMMAND camstream-m5-service-test
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:camstream-camera-backend-faults>
        $<TARGET_FILE:gstcamstreamsrc>)

ADD_TEST(NAME camstream-m5-service-smoke-simulated
    COMMAND camstream-m5-service-smoke
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:gstcamstreamsrc>
        simulated0 64 48 30 1 3 2)
