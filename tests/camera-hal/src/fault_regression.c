#include <camstream/camera/camera_hal.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "HAL C regression failed at line %d: %s\n", __LINE__, #condition);                         \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int prepare_camera(camstream_camera* camera, const char* source)
{
    CHECK(camstream_camera_open(camera, source) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_capabilities_v1 capabilities = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(capabilities),
    };
    CHECK(camstream_camera_get_capabilities(camera, &capabilities) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(capabilities.stream_config_count == 1U);
    camstream_camera_stream_config_v1 requested = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(requested),
    };
    CHECK(camstream_camera_get_stream_configuration(camera, 0U, &requested) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_stream_config_v1 active = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(active),
    };
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

static int test_failed_release(const char* module)
{
    CHECK(camstream_camera_hal_load_backend(module) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(prepare_camera(camera, "release-fail") == 0);
    camstream_camera_frame_v1 frame = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(frame),
    };
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(frame.frame_token != 0U);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

static int test_pending_cleanup(const char* module)
{
    CHECK(camstream_camera_hal_load_backend(module) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(prepare_camera(camera, "invalid-frame") == 0);
    camstream_camera_frame_v1 frame = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(frame),
            .frame_token = UINT64_C(777),
    };
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(frame.frame_token == UINT64_C(777));
    CHECK(camstream_camera_wait_frame(camera, 0U) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

static int test_public_header_validation(const char* module)
{
    CHECK(camstream_camera_create(NULL) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_hal_load_backend(NULL) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_hal_load_backend(module) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_hal_load_backend(module) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_open(camera, "release-fail") == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_capabilities_v1 capabilities = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(capabilities) - 1U,
    };
    CHECK(camstream_camera_get_capabilities(camera, &capabilities) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    struct
    {
        camstream_camera_capabilities_v1 base;
        uint8_t extension[8];
    } extended_capabilities = {
            .base =
                    {
                            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                            .struct_size = sizeof(extended_capabilities),
                    },
    };
    CHECK(camstream_camera_get_capabilities(camera, &extended_capabilities.base) == CAMSTREAM_CAMERA_STATUS_OK);
    capabilities.struct_size = sizeof(capabilities);
    capabilities.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1 + 1U;
    CHECK(camstream_camera_get_capabilities(camera, &capabilities) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

static int test_private_token_exhaustion(const char* module)
{
    CHECK(camstream_camera_hal_load_backend(module) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(prepare_camera(camera, "simulated0") == 0);

    camstream_camera_frame_v1 frame = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(frame),
    };
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(frame.frame_token != 0U);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);

    frame.frame_token = UINT64_C(777);
    CHECK(camstream_camera_wait_frame(camera, 0U) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(frame.frame_token == UINT64_C(777));
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        return 2;
    }
    if (test_public_header_validation(argv[1]) != 0 || test_failed_release(argv[1]) != 0 ||
            test_pending_cleanup(argv[1]) != 0 || test_private_token_exhaustion(argv[2]) != 0)
    {
        return 1;
    }
    puts("Camera HAL C fault regressions: PASS");
    return 0;
}
