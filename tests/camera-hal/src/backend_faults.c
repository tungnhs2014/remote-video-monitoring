#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_backend.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct camstream_camera_instance
{
    bool opened;
    bool configured;
    bool started;
    bool outstanding;
    bool fail_release_once;
    bool invalid_frame;
    bool fail_acquire;
    bool wait_timeout;
    bool blocked_wait;
    bool gated_release;
    bool padded;
    uint64_t next_sequence;
    uint8_t payload[16];
};

static atomic_bool blocked_wait_active = ATOMIC_VAR_INIT(false);
static atomic_bool blocked_wait_release = ATOMIC_VAR_INIT(false);
static atomic_bool release_failed = ATOMIC_VAR_INIT(false);

bool camstream_test_wait_active(void)
{
    return atomic_load_explicit(&blocked_wait_active, memory_order_acquire);
}

void camstream_test_release_wait(void)
{
    atomic_store_explicit(&blocked_wait_release, true, memory_order_release);
}

bool camstream_test_release_failed(void)
{
    return atomic_load_explicit(&release_failed, memory_order_acquire);
}

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
    if (strcmp(source, "release-fail") != 0 && strcmp(source, "invalid-frame") != 0 &&
            strcmp(source, "acquire-fail") != 0 && strcmp(source, "wait-timeout") != 0 &&
            strcmp(source, "blocked-wait") != 0 && strcmp(source, "gated-release") != 0 &&
            strcmp(source, "padded") != 0)
    {
        return CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
    }
    instance->opened = true;
    instance->fail_release_once = strcmp(source, "release-fail") == 0 || strcmp(source, "invalid-frame") == 0 ||
                                  strcmp(source, "gated-release") == 0;
    instance->invalid_frame = strcmp(source, "invalid-frame") == 0;
    instance->fail_acquire = strcmp(source, "acquire-fail") == 0;
    instance->wait_timeout = strcmp(source, "wait-timeout") == 0;
    instance->blocked_wait = strcmp(source, "blocked-wait") == 0 || strcmp(source, "gated-release") == 0;
    instance->gated_release = strcmp(source, "gated-release") == 0;
    atomic_store_explicit(&release_failed, false, memory_order_release);
    if (instance->blocked_wait)
    {
        atomic_store_explicit(&blocked_wait_release, false, memory_order_release);
    }
    instance->padded = strcmp(source, "padded") == 0;
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
    fill_configuration(configuration, instance->invalid_frame   ? "invalid-frame"
                                      : instance->fail_acquire  ? "acquire-fail"
                                      : instance->wait_timeout  ? "wait-timeout"
                                      : instance->gated_release ? "gated-release"
                                      : instance->blocked_wait  ? "blocked-wait"
                                      : instance->padded        ? "padded"
                                                                : "release-fail");
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
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->started)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (instance->blocked_wait && instance->next_sequence > 0U)
    {
        atomic_store_explicit(&blocked_wait_active, true, memory_order_release);
        for (uint32_t attempt = 0U;
                attempt < 5000U && !atomic_load_explicit(&blocked_wait_release, memory_order_acquire); ++attempt)
        {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            nanosleep(&delay, NULL);
        }
        atomic_store_explicit(&blocked_wait_active, false, memory_order_release);
        return instance->gated_release ? CAMSTREAM_CAMERA_STATUS_OK : CAMSTREAM_CAMERA_STATUS_TIMEOUT;
    }
    if (instance->wait_timeout)
    {
        struct timespec delay = {.tv_sec = timeout_ms / 1000U, .tv_nsec = (long)(timeout_ms % 1000U) * 1000000L};
        nanosleep(&delay, NULL);
        return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
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
    if (instance->fail_acquire)
    {
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    memset(frame, 0, sizeof(*frame));
    frame->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->struct_size = sizeof(*frame);
    frame->frame_token = 1U;
    frame->sequence_number = instance->next_sequence;
    instance->next_sequence += instance->padded ? 2U : 1U;
    frame->width = 2U;
    frame->height = 2U;
    frame->pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    frame->plane_count = 1U;
    frame->planes[0].abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->planes[0].struct_size = sizeof(frame->planes[0]);
    frame->planes[0].data = instance->payload;
    frame->planes[0].allocation_size = instance->padded ? sizeof(instance->payload) : 8U;
    frame->planes[0].bytes_used = instance->invalid_frame ? 0U : frame->planes[0].allocation_size;
    frame->planes[0].stride = instance->padded ? 8U : 4U;
    if (instance->padded)
    {
        memset(instance->payload, 0xee, sizeof(instance->payload));
        for (uint32_t index = 0U; index < 8U; ++index)
        {
            instance->payload[index + (index >= 4U ? 4U : 0U)] = (uint8_t)(frame->sequence_number + index);
        }
    }
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
    if (instance->fail_release_once && (!instance->gated_release || instance->next_sequence > 1U))
    {
        instance->fail_release_once = false;
        atomic_store_explicit(&release_failed, true, memory_order_release);
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

__attribute__((constructor)) static void register_fault_backend(void)
{
    camstream_camera_binding_v2* binding = NULL;
    if (camstream_camera_hal_get_binding_slot_v2(&binding, sizeof(camstream_camera_binding_v2),
                sizeof(camstream_camera_ops_v2)) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return;
    }
    *binding->control_funcs = (camstream_camera_ops_v2){
            .struct_size = sizeof(camstream_camera_ops_v2),
            .camera_create = fault_create,
            .camera_destroy = fault_destroy,
            .camera_open = fault_open,
            .camera_close = fault_close,
            .camera_get_capabilities = fault_get_capabilities,
            .camera_get_stream_configuration = fault_get_configuration,
            .camera_configure = fault_configure,
            .camera_start = fault_start,
            .camera_wait_frame = fault_wait,
            .camera_acquire_frame = fault_acquire,
            .camera_release_frame = fault_release,
            .camera_stop = fault_stop,
            .camera_get_last_error = fault_get_error,
    };
    binding->backend_name = "fault-test";
    binding->struct_size = sizeof(*binding);
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
}
