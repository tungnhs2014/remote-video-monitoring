IF(NOT CAMSTREAM_ENABLE_GRPC)
    RETURN()
ENDIF()

FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
PKG_CHECK_MODULES(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)

FIND_PROGRAM(CAMSTREAM_PROTOC_EXECUTABLE NAMES protoc REQUIRED)
FIND_PROGRAM(CAMSTREAM_GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin REQUIRED)

SET(CAMSTREAM_GRPC_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/camera_control.proto)
SET(CAMSTREAM_GRPC_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
FILE(MAKE_DIRECTORY ${CAMSTREAM_GRPC_GENERATED_DIR})

ADD_CUSTOM_COMMAND(
    OUTPUT
        ${CAMSTREAM_GRPC_GENERATED_DIR}/camera_control.pb.cc
        ${CAMSTREAM_GRPC_GENERATED_DIR}/camera_control.pb.h
        ${CAMSTREAM_GRPC_GENERATED_DIR}/camera_control.grpc.pb.cc
        ${CAMSTREAM_GRPC_GENERATED_DIR}/camera_control.grpc.pb.h
    COMMAND ${CAMSTREAM_PROTOC_EXECUTABLE}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${CAMSTREAM_GRPC_GENERATED_DIR}
        --grpc_out=${CAMSTREAM_GRPC_GENERATED_DIR}
        --plugin=protoc-gen-grpc=${CAMSTREAM_GRPC_CPP_PLUGIN_EXECUTABLE}
        ${CAMSTREAM_GRPC_PROTO}
    DEPENDS ${CAMSTREAM_GRPC_PROTO}
    VERBATIM)

ADD_LIBRARY(camstream-grpc-proto STATIC
    ${CAMSTREAM_GRPC_GENERATED_DIR}/camera_control.pb.cc
    ${CAMSTREAM_GRPC_GENERATED_DIR}/camera_control.grpc.pb.cc)
TARGET_COMPILE_FEATURES(camstream-grpc-proto PUBLIC cxx_std_17)
TARGET_INCLUDE_DIRECTORIES(camstream-grpc-proto PUBLIC ${CAMSTREAM_GRPC_GENERATED_DIR})
TARGET_LINK_LIBRARIES(camstream-grpc-proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)

ADD_LIBRARY(camstream-grpc-adapter STATIC src/grpc_adapter.cpp)
TARGET_COMPILE_FEATURES(camstream-grpc-adapter PUBLIC cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream-grpc-adapter PRIVATE -Wall -Wextra -Wpedantic)
TARGET_INCLUDE_DIRECTORIES(camstream-grpc-adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
TARGET_LINK_LIBRARIES(camstream-grpc-adapter PUBLIC camstream-grpc-proto camstream::camera-service
    PRIVATE camstream::common-log)

ADD_EXECUTABLE(camstream-grpc-server src/server_main.cpp)
TARGET_COMPILE_FEATURES(camstream-grpc-server PRIVATE cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream-grpc-server PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-grpc-server PRIVATE camstream-grpc-adapter camstream::common-log)

ADD_EXECUTABLE(camstream-grpc-client src/client_main.cpp)
TARGET_COMPILE_FEATURES(camstream-grpc-client PRIVATE cxx_std_17)
TARGET_COMPILE_OPTIONS(camstream-grpc-client PRIVATE -Wall -Wextra -Wpedantic)
TARGET_LINK_LIBRARIES(camstream-grpc-client PRIVATE camstream-grpc-proto)

INSTALL(TARGETS camstream-grpc-server camstream-grpc-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

IF(BUILD_TESTING)
    TARGET_COMPILE_DEFINITIONS(camstream-grpc-adapter PUBLIC CAMSTREAM_M6_TESTING)
    ADD_EXECUTABLE(camstream-grpc-integration-test tests/integration_test.cpp)
    TARGET_COMPILE_FEATURES(camstream-grpc-integration-test PRIVATE cxx_std_17)
    TARGET_COMPILE_OPTIONS(camstream-grpc-integration-test PRIVATE -Wall -Wextra -Wpedantic)
    TARGET_LINK_LIBRARIES(camstream-grpc-integration-test PRIVATE camstream-grpc-adapter)
    ADD_DEPENDENCIES(camstream-grpc-integration-test camstream-camera-backend-simulated gstcamstreamsrc)
    ADD_TEST(NAME camstream-grpc-integration
        COMMAND camstream-grpc-integration-test
            $<TARGET_FILE:camstream-camera-backend-simulated>
            $<TARGET_FILE:gstcamstreamsrc>)
    SET_TESTS_PROPERTIES(camstream-grpc-integration PROPERTIES TIMEOUT 30)
ENDIF()
