add_library(camstream-camera-backend-simulated MODULE src/camera_backend_simulated.c)
target_compile_features(camstream-camera-backend-simulated PRIVATE c_std_11)
target_compile_options(camstream-camera-backend-simulated PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-backend-simulated PRIVATE camstream::camera-hal)

install(TARGETS camstream-camera-backend-simulated
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/camstream/backends)
