ADD_LIBRARY(camstream-camera-backend-v4l2 MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/camera_backend_v4l2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v4l2_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v4l2_config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v4l2_stream.c)
SET_TARGET_PROPERTIES(camstream-camera-backend-v4l2 PROPERTIES C_VISIBILITY_PRESET hidden)
TARGET_COMPILE_FEATURES(camstream-camera-backend-v4l2 PRIVATE c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-backend-v4l2 PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-backend-v4l2 PRIVATE camstream::camera-hal camstream::common-log)

INSTALL(TARGETS camstream-camera-backend-v4l2
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/camstream/backends)
