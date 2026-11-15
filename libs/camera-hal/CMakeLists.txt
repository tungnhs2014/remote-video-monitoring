find_package(Threads REQUIRED)

add_library(camstream-camera-hal SHARED
    src/camera_backend_runtime.c
    src/camera_hal.c
)
add_library(camstream::camera-hal ALIAS camstream-camera-hal)

target_include_directories(camstream-camera-hal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(camstream-camera-hal PUBLIC c_std_11)
target_compile_options(camstream-camera-hal PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-camera-hal PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

install(TARGETS camstream-camera-hal LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
