FIND_PACKAGE(Threads REQUIRED)

ADD_EXECUTABLE(camstream-camera-hal-regression-tests src/main.cpp)

SET_TARGET_PROPERTIES(camstream-camera-hal-regression-tests PROPERTIES ENABLE_EXPORTS YES)
TARGET_COMPILE_FEATURES(camstream-camera-hal-regression-tests PRIVATE cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream-camera-hal-regression-tests PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-hal-regression-tests PRIVATE
    camstream::camera-hal Threads::Threads)

ADD_LIBRARY(camstream-camera-backend-no-registration MODULE src/backend_no_registration.cpp)
ADD_LIBRARY(camstream-camera-backend-invalid-descriptor MODULE src/backend_invalid_descriptor.cpp)
ADD_LIBRARY(camstream-camera-backend-duplicate-registration MODULE
    src/backend_duplicate_registration.cpp)
ADD_LIBRARY(camstream-camera-backend-blocking-create MODULE src/backend_blocking_create.cpp)
ADD_LIBRARY(camstream-camera-backend-faults MODULE src/backend_faults.c)

FOREACH(binding_failure_mode RANGE 1 5)
    ADD_LIBRARY(camstream-camera-backend-binding-failure-${binding_failure_mode} MODULE
        src/backend_binding_failure.c)
    TARGET_COMPILE_DEFINITIONS(camstream-camera-backend-binding-failure-${binding_failure_mode}
        PRIVATE CAMSTREAM_BINDING_FAILURE_MODE=${binding_failure_mode})
    TARGET_COMPILE_FEATURES(camstream-camera-backend-binding-failure-${binding_failure_mode}
        PRIVATE c_std_11)
    TARGET_COMPILE_OPTIONS(camstream-camera-backend-binding-failure-${binding_failure_mode}
        PRIVATE -Wall -Wextra -Wpedantic)
    TARGET_LINK_LIBRARIES(camstream-camera-backend-binding-failure-${binding_failure_mode}
        PRIVATE camstream::camera-hal)
ENDFOREACH()

ADD_LIBRARY(camstream-camera-backend-legacy-hook MODULE src/backend_legacy_hook.c)

TARGET_COMPILE_FEATURES(camstream-camera-backend-legacy-hook PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-backend-legacy-hook PRIVATE -Wall -Wextra -Wpedantic)

TARGET_COMPILE_FEATURES(camstream-camera-backend-faults PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-backend-faults PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-backend-faults PRIVATE camstream::camera-hal)

ADD_LIBRARY(camstream-camera-backend-simulated-exhaustion MODULE
    ../../backends/camera/simulated/src/camera_backend_simulated.c)

TARGET_COMPILE_DEFINITIONS(camstream-camera-backend-simulated-exhaustion PRIVATE
    CAMSTREAM_SIMULATED_TEST_START_AT_MAX)
TARGET_COMPILE_FEATURES(camstream-camera-backend-simulated-exhaustion PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-backend-simulated-exhaustion PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-backend-simulated-exhaustion PRIVATE camstream::camera-hal)

ADD_EXECUTABLE(camstream-camera-hal-fault-tests src/fault_regression.c)

TARGET_COMPILE_FEATURES(camstream-camera-hal-fault-tests PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-hal-fault-tests PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-hal-fault-tests PRIVATE camstream::camera-hal)

ADD_DEPENDENCIES(camstream-camera-hal-fault-tests
    camstream-camera-backend-faults
    camstream-camera-backend-simulated-exhaustion)
ADD_TEST(NAME camera-hal-fault-regressions
    COMMAND camstream-camera-hal-fault-tests
        $<TARGET_FILE:camstream-camera-backend-faults>
        $<TARGET_FILE:camstream-camera-backend-simulated-exhaustion>)

FOREACH(test_backend
        camstream-camera-backend-no-registration
        camstream-camera-backend-invalid-descriptor
        camstream-camera-backend-duplicate-registration
        camstream-camera-backend-blocking-create)
    TARGET_COMPILE_FEATURES(${test_backend} PRIVATE cxx_std_17)
    TARGET_COMPILE_OPTIONS(${test_backend} PRIVATE -Wall -Wextra -Wpedantic)
ENDFOREACH()

TARGET_LINK_LIBRARIES(camstream-camera-backend-invalid-descriptor PRIVATE camstream::camera-hal)
TARGET_LINK_LIBRARIES(camstream-camera-backend-duplicate-registration PRIVATE camstream::camera-hal)
TARGET_LINK_LIBRARIES(camstream-camera-backend-blocking-create PRIVATE camstream::camera-hal)

ADD_DEPENDENCIES(camstream-camera-hal-regression-tests
    camstream-camera-backend-simulated
    camstream-camera-backend-no-registration
    camstream-camera-backend-invalid-descriptor
    camstream-camera-backend-duplicate-registration
    camstream-camera-backend-blocking-create
    camstream-camera-backend-binding-failure-1
    camstream-camera-backend-binding-failure-2
    camstream-camera-backend-binding-failure-3
    camstream-camera-backend-binding-failure-4
    camstream-camera-backend-binding-failure-5
    camstream-camera-backend-legacy-hook
)

ADD_TEST(NAME camera-hal-runtime-regressions
    COMMAND camstream-camera-hal-regression-tests
        $<TARGET_FILE:camstream-camera-backend-simulated>
        $<TARGET_FILE:camstream-camera-backend-no-registration>
        $<TARGET_FILE:camstream-camera-backend-invalid-descriptor>
        $<TARGET_FILE:camstream-camera-backend-duplicate-registration>
        $<TARGET_FILE:camstream-camera-backend-blocking-create>
        $<TARGET_FILE:camstream-camera-backend-binding-failure-1>
        $<TARGET_FILE:camstream-camera-backend-binding-failure-2>
        $<TARGET_FILE:camstream-camera-backend-binding-failure-3>
        $<TARGET_FILE:camstream-camera-backend-binding-failure-4>
        $<TARGET_FILE:camstream-camera-backend-binding-failure-5>
        $<TARGET_FILE:camstream-camera-backend-legacy-hook>
)

ADD_EXECUTABLE(camstream-camera-runtime-dlclose-fault src/runtime_dlclose_fault.c)

SET_TARGET_PROPERTIES(camstream-camera-runtime-dlclose-fault PROPERTIES ENABLE_EXPORTS YES)
TARGET_COMPILE_FEATURES(camstream-camera-runtime-dlclose-fault PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-runtime-dlclose-fault PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-runtime-dlclose-fault PRIVATE camstream::camera-hal)
ADD_DEPENDENCIES(camstream-camera-runtime-dlclose-fault camstream-camera-backend-simulated)
ADD_TEST(NAME camera-hal-dlclose-fault
    COMMAND camstream-camera-runtime-dlclose-fault $<TARGET_FILE:camstream-camera-backend-simulated>)

ADD_LIBRARY(camstream-camera-backend-reentrant-valid MODULE
    src/backend_reentrant_transition.c
    src/backend_faults.c)
ADD_LIBRARY(camstream-camera-backend-reentrant-invalid MODULE
    src/backend_reentrant_transition.c
    src/backend_binding_failure.c)

TARGET_COMPILE_DEFINITIONS(camstream-camera-backend-reentrant-invalid PRIVATE
    CAMSTREAM_BINDING_FAILURE_MODE=4)

FOREACH(reentrant_backend IN ITEMS
        camstream-camera-backend-reentrant-valid
        camstream-camera-backend-reentrant-invalid)
    TARGET_COMPILE_FEATURES(${reentrant_backend} PRIVATE c_std_11)
    TARGET_COMPILE_OPTIONS(${reentrant_backend} PRIVATE -Wall -Wextra -Wpedantic)
    TARGET_LINK_LIBRARIES(${reentrant_backend} PRIVATE camstream::camera-hal)
ENDFOREACH()

ADD_EXECUTABLE(camstream-camera-hal-reentrant-test src/reentrant_loader_test.c)

SET_TARGET_PROPERTIES(camstream-camera-hal-reentrant-test PROPERTIES ENABLE_EXPORTS YES)
TARGET_COMPILE_FEATURES(camstream-camera-hal-reentrant-test PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-hal-reentrant-test PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-hal-reentrant-test PRIVATE
    camstream::camera-hal Threads::Threads)
ADD_DEPENDENCIES(camstream-camera-hal-reentrant-test
    camstream-camera-backend-reentrant-valid
    camstream-camera-backend-reentrant-invalid)
ADD_TEST(NAME camera-hal-reentrant-loader
    COMMAND camstream-camera-hal-reentrant-test
        $<TARGET_FILE:camstream-camera-backend-reentrant-valid>
        $<TARGET_FILE:camstream-camera-backend-reentrant-invalid>)
SET_TESTS_PROPERTIES(camera-hal-reentrant-loader PROPERTIES TIMEOUT 15)

GET_TARGET_PROPERTY(CAMSTREAM_V4L2_SOURCES camstream-camera-backend-v4l2 SOURCES)
ADD_LIBRARY(camstream-camera-backend-v4l2-fake MODULE ${CAMSTREAM_V4L2_SOURCES})

SET_TARGET_PROPERTIES(camstream-camera-backend-v4l2-fake PROPERTIES C_VISIBILITY_PRESET hidden)
TARGET_COMPILE_DEFINITIONS(camstream-camera-backend-v4l2-fake PRIVATE CAMSTREAM_V4L2_TEST_IO)
TARGET_COMPILE_FEATURES(camstream-camera-backend-v4l2-fake PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-backend-v4l2-fake PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-backend-v4l2-fake PRIVATE
    camstream::camera-hal camstream::common-log)

ADD_EXECUTABLE(camstream-camera-v4l2-phase1-tests src/v4l2_phase1_regression.c)

SET_TARGET_PROPERTIES(camstream-camera-v4l2-phase1-tests PROPERTIES ENABLE_EXPORTS YES)
TARGET_COMPILE_FEATURES(camstream-camera-v4l2-phase1-tests PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-v4l2-phase1-tests PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-v4l2-phase1-tests PRIVATE camstream::camera-hal)

ADD_DEPENDENCIES(camstream-camera-v4l2-phase1-tests camstream-camera-backend-v4l2-fake)
ADD_TEST(NAME camera-v4l2-phase1-regressions
    COMMAND camstream-camera-v4l2-phase1-tests $<TARGET_FILE:camstream-camera-backend-v4l2-fake>)

ADD_TEST(NAME camera-v4l2-configuration-regressions
    COMMAND camstream-camera-v4l2-phase1-tests
        $<TARGET_FILE:camstream-camera-backend-v4l2-fake> --configuration)

ADD_TEST(NAME camera-v4l2-streaming-regressions
    COMMAND camstream-camera-v4l2-phase1-tests
        $<TARGET_FILE:camstream-camera-backend-v4l2-fake> --streaming)

ADD_EXECUTABLE(camstream-camera-v4l2-device-smoke src/v4l2_device_smoke.c)

TARGET_COMPILE_FEATURES(camstream-camera-v4l2-device-smoke PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-v4l2-device-smoke PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-v4l2-device-smoke PRIVATE camstream::camera-hal)
