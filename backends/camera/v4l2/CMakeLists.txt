add_library(camstream-camera-backend-v4l2 MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/camera_backend_v4l2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v4l2_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v4l2_config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v4l2_stream.c)
set_target_properties(camstream-camera-backend-v4l2 PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_features(camstream-camera-backend-v4l2 PRIVATE c_std_11)
target_compile_options(camstream-camera-backend-v4l2 PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-backend-v4l2 PRIVATE camstream::camera-hal camstream::common-log)

install(TARGETS camstream-camera-backend-v4l2
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/camstream/backends)
