ADD_LIBRARY(camstream-camera-backend-simulated MODULE src/camera_backend_simulated.c)
TARGET_COMPILE_FEATURES(camstream-camera-backend-simulated PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-backend-simulated PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-backend-simulated PRIVATE camstream::camera-hal)

INSTALL(TARGETS camstream-camera-backend-simulated
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/camstream/backends)
