add_library(camstream-camera-service STATIC src/camera_service.cpp)
add_library(camstream::camera-service ALIAS camstream-camera-service)
target_compile_features(camstream-camera-service PUBLIC cxx_std_17)
target_compile_options(camstream-camera-service PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(camstream-camera-service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(camstream-camera-service PUBLIC camstream::gstreamer-controller camstream::camera-hal
    PRIVATE camstream::common-log)

add_executable(camstream-m5-service-smoke tests/m5_service_smoke.cpp)
target_compile_features(camstream-m5-service-smoke PRIVATE cxx_std_17)
target_compile_options(camstream-m5-service-smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camstream-m5-service-smoke PRIVATE camstream::camera-service)
install(TARGETS camstream-m5-service-smoke RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
