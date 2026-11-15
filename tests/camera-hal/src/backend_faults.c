#include <camstream/camera/camera_backend.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct camstream_camera_instance
{
    bool opened;
    bool configured;
    bool started;
    bool outstanding;
    bool fail_release_once;
    bool invalid_frame;
    uint8_t payload[8];
};

static camstream_camera_status_t fault_create(camstream_camera_instance** instance)
{
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *instance = calloc(1U, sizeof(**instance));
    return *instance == NULL ? CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR : CAMSTREAM_CAMERA_STATUS_OK;
}

static void fault_destroy(camstream_camera_instance* instance)
{
    free(instance);
}

static camstream_camera_status_t fault_open(camstream_camera_instance* instance, const char* source)
{
    if (instance == NULL || source == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (instance->opened)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (strcmp(source, "release-fail") != 0 && strcmp(source, "invalid-frame") != 0)
    {
        return CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
    }
    instance->opened = true;
    instance->fail_release_once = true;
    instance->invalid_frame = strcmp(source, "invalid-frame") == 0;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_close(camstream_camera_instance* instance)
{
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (instance->started || instance->outstanding)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    instance->opened = false;
    instance->configured = false;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_get_capabilities(
        camstream_camera_instance* instance, camstream_camera_capabilities_v1* capabilities)
{
    if (instance == NULL || capabilities == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->opened)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    capabilities->struct_size = sizeof(*capabilities);
    capabilities->stream_config_count = 1U;
    capabilities->maximum_plane_count = 1U;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static void fill_configuration(camstream_camera_stream_config_v1* configuration, const char* source)
{
    memset(configuration, 0, sizeof(*configuration));
    configuration->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    configuration->struct_size = sizeof(*configuration);
    memcpy(configuration->source_identifier, source, strlen(source) + 1U);
    configuration->width = 2U;
    configuration->height = 2U;
    configuration->pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    configuration->frame_rate_numerator = 30U;
    configuration->frame_rate_denominator = 1U;
}

static camstream_camera_status_t fault_get_configuration(
        camstream_camera_instance* instance, uint32_t index, camstream_camera_stream_config_v1* configuration)
{
    if (instance == NULL || configuration == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->opened || instance->configured)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (index != 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    fill_configuration(configuration, instance->invalid_frame ? "invalid-frame" : "release-fail");
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_configure(camstream_camera_instance* instance,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    if (instance == NULL || requested == NULL || active == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->opened || instance->configured)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    fill_configuration(active, requested->source_identifier);
    instance->configured = true;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_start(camstream_camera_instance* instance)
{
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->configured)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    instance->started = true;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_wait(camstream_camera_instance* instance, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    return instance->started ? CAMSTREAM_CAMERA_STATUS_OK : CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
}

static camstream_camera_status_t fault_acquire(camstream_camera_instance* instance, camstream_camera_frame_v1* frame)
{
    if (instance == NULL || frame == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->started || instance->outstanding)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    memset(frame, 0, sizeof(*frame));
    frame->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->struct_size = sizeof(*frame);
    frame->frame_token = 1U;
    frame->width = 2U;
    frame->height = 2U;
    frame->pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    frame->plane_count = 1U;
    frame->planes[0].abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->planes[0].struct_size = sizeof(frame->planes[0]);
    frame->planes[0].data = instance->payload;
    frame->planes[0].allocation_size = sizeof(instance->payload);
    frame->planes[0].bytes_used = instance->invalid_frame ? 0U : sizeof(instance->payload);
    frame->planes[0].stride = 4U;
    instance->outstanding = true;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_release(camstream_camera_instance* instance, uint64_t token)
{
    if (instance == NULL || token == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->outstanding || token != 1U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (instance->fail_release_once)
    {
        instance->fail_release_once = false;
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    instance->outstanding = false;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_stop(camstream_camera_instance* instance)
{
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->started || instance->outstanding)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    instance->started = false;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t fault_get_error(camstream_camera_instance* instance, char* buffer, uint32_t size)
{
    if (instance == NULL || buffer == NULL || size == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    buffer[0] = '\0';
    return CAMSTREAM_CAMERA_STATUS_OK;
}

typedef struct extended_descriptor
{
    camstream_camera_backend_v1 base;
    uint8_t extension[8];
} extended_descriptor;

static const extended_descriptor descriptor = {
        .base =
                {
                        .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                        .struct_size = sizeof(extended_descriptor),
                        .backend_name = "fault-test",
                        .create = fault_create,
                        .destroy = fault_destroy,
                        .open = fault_open,
                        .close = fault_close,
                        .get_capabilities = fault_get_capabilities,
                        .get_stream_configuration = fault_get_configuration,
                        .configure = fault_configure,
                        .start = fault_start,
                        .wait_frame = fault_wait,
                        .acquire_frame = fault_acquire,
                        .release_frame = fault_release,
                        .stop = fault_stop,
                        .get_last_error = fault_get_error,
                },
        .extension = {0},
};

__attribute__((constructor)) static void register_fault_backend(void)
{
    (void)camstream_camera_hal_register_backend_v1(&descriptor.base);
}
