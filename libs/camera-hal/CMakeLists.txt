FIND_PACKAGE(Threads REQUIRED)

ADD_LIBRARY(camstream-camera-hal SHARED
    src/camera_backend_runtime.c
    src/camera_core.c
    src/camera_hal.c
)
SET_SOURCE_FILES_PROPERTIES(src/camera_core.c PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
ADD_LIBRARY(camstream::camera-hal ALIAS camstream-camera-hal)

TARGET_INCLUDE_DIRECTORIES(camstream-camera-hal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
TARGET_COMPILE_FEATURES(camstream-camera-hal PUBLIC c_std_11)
TARGET_COMPILE_OPTIONS(camstream-camera-hal PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-camera-hal PRIVATE ${CMAKE_DL_LIBS} Threads::Threads camstream::common-log)
TARGET_LINK_OPTIONS(camstream-camera-hal PRIVATE -Wl,--exclude-libs,libcamstream-common-log.a)

INSTALL(TARGETS camstream-camera-hal LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
INSTALL(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
