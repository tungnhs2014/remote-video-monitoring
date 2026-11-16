find_package(Threads REQUIRED)

add_executable(camstream-camera-hal-regression-tests src/main.cpp)
set_target_properties(camstream-camera-hal-regression-tests PROPERTIES ENABLE_EXPORTS YES)
target_compile_features(camstream-camera-hal-regression-tests PRIVATE cxx_std_17)
target_compile_options(camstream-camera-hal-regression-tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-hal-regression-tests PRIVATE camstream::camera-hal Threads::Threads)

add_library(camstream-camera-backend-no-registration MODULE src/backend_no_registration.cpp)
add_library(camstream-camera-backend-invalid-descriptor MODULE src/backend_invalid_descriptor.cpp)
add_library(camstream-camera-backend-duplicate-registration MODULE src/backend_duplicate_registration.cpp)
add_library(camstream-camera-backend-blocking-create MODULE src/backend_blocking_create.cpp)
add_library(camstream-camera-backend-faults MODULE src/backend_faults.c)
target_compile_features(camstream-camera-backend-faults PRIVATE c_std_11)
target_compile_options(camstream-camera-backend-faults PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-backend-faults PRIVATE camstream::camera-hal)

add_library(camstream-camera-backend-simulated-exhaustion MODULE
    ../../backends/camera/simulated/src/camera_backend_simulated.c)
target_compile_definitions(camstream-camera-backend-simulated-exhaustion PRIVATE CAMSTREAM_SIMULATED_TEST_START_AT_MAX)
target_compile_features(camstream-camera-backend-simulated-exhaustion PRIVATE c_std_11)
target_compile_options(camstream-camera-backend-simulated-exhaustion PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-backend-simulated-exhaustion PRIVATE camstream::camera-hal)

add_executable(camstream-camera-hal-fault-tests src/fault_regression.c)
target_compile_features(camstream-camera-hal-fault-tests PRIVATE c_std_11)
target_compile_options(camstream-camera-hal-fault-tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-hal-fault-tests PRIVATE camstream::camera-hal)
add_dependencies(camstream-camera-hal-fault-tests
    camstream-camera-backend-faults
    camstream-camera-backend-simulated-exhaustion)
add_test(NAME camera-hal-fault-regressions
    COMMAND camstream-camera-hal-fault-tests
        $<TARGET_FILE:camstream-camera-backend-faults>
        $<TARGET_FILE:camstream-camera-backend-simulated-exhaustion>)

foreach(test_backend
        camstream-camera-backend-no-registration
        camstream-camera-backend-invalid-descriptor
        camstream-camera-backend-duplicate-registration
        camstream-camera-backend-blocking-create)
    target_compile_features(${test_backend} PRIVATE cxx_std_17)
    target_compile_options(${test_backend} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

target_link_libraries(camstream-camera-backend-invalid-descriptor PRIVATE camstream::camera-hal)
target_link_libraries(camstream-camera-backend-duplicate-registration PRIVATE camstream::camera-hal)
target_link_libraries(camstream-camera-backend-blocking-create PRIVATE camstream::camera-hal)

add_dependencies(camstream-camera-hal-regression-tests
    camstream-camera-backend-simulated
    camstream-camera-backend-no-registration
    camstream-camera-backend-invalid-descriptor
    camstream-camera-backend-duplicate-registration
    camstream-camera-backend-blocking-create
)

add_test(NAME camera-hal-runtime-regressions
    COMMAND camstream-camera-hal-regression-tests
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:camstream-camera-backend-no-registration>
        $<TARGET_FILE:camstream-camera-backend-invalid-descriptor>
        $<TARGET_FILE:camstream-camera-backend-duplicate-registration>
        $<TARGET_FILE:camstream-camera-backend-blocking-create>
)

add_library(camstream-camera-backend-v4l2-fake MODULE
    ../../backends/camera/v4l2/src/camera_backend_v4l2.c)
target_compile_definitions(camstream-camera-backend-v4l2-fake PRIVATE CAMSTREAM_V4L2_TEST_IO)
target_compile_features(camstream-camera-backend-v4l2-fake PRIVATE c_std_11)
target_compile_options(camstream-camera-backend-v4l2-fake PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-backend-v4l2-fake PRIVATE camstream::camera-hal camstream::common-log)

add_executable(camstream-camera-v4l2-phase1-tests src/v4l2_phase1_regression.c)
set_target_properties(camstream-camera-v4l2-phase1-tests PROPERTIES ENABLE_EXPORTS YES)
target_compile_features(camstream-camera-v4l2-phase1-tests PRIVATE c_std_11)
target_compile_options(camstream-camera-v4l2-phase1-tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-v4l2-phase1-tests PRIVATE camstream::camera-hal)
add_dependencies(camstream-camera-v4l2-phase1-tests camstream-camera-backend-v4l2-fake)
add_test(NAME camera-v4l2-phase1-regressions
    COMMAND camstream-camera-v4l2-phase1-tests $<TARGET_FILE:camstream-camera-backend-v4l2-fake>)
add_test(NAME camera-v4l2-configuration-regressions
    COMMAND camstream-camera-v4l2-phase1-tests $<TARGET_FILE:camstream-camera-backend-v4l2-fake> --configuration)
add_test(NAME camera-v4l2-streaming-regressions
    COMMAND camstream-camera-v4l2-phase1-tests $<TARGET_FILE:camstream-camera-backend-v4l2-fake> --streaming)

add_executable(camstream-camera-v4l2-device-smoke src/v4l2_device_smoke.c)
target_compile_features(camstream-camera-v4l2-device-smoke PRIVATE c_std_11)
target_compile_options(camstream-camera-v4l2-device-smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-v4l2-device-smoke PRIVATE camstream::camera-hal)
