add_library(camstream-camera-backend-v4l2 MODULE src/camera_backend_v4l2.c)
target_compile_features(camstream-camera-backend-v4l2 PRIVATE c_std_11)
target_compile_options(camstream-camera-backend-v4l2 PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-backend-v4l2 PRIVATE camstream::camera-hal)

install(TARGETS camstream-camera-backend-v4l2
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/camstream/backends)
