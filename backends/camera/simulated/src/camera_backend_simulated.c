#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_backend.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef CAMSTREAM_SIMULATED_TEST_START_AT_MAX
#define SIM_INITIAL_TOKEN UINT64_MAX
#else
#define SIM_INITIAL_TOKEN UINT64_C(1)
#endif

enum
{
    SIM_WIDTH = 64,
    SIM_HEIGHT = 48,
    SIM_STRIDE = SIM_WIDTH * 2,
    SIM_FRAME_SIZE = SIM_STRIDE * SIM_HEIGHT,
    SIM_BUFFER_COUNT = 4
};

static const char backend_name[] = "simulated";
static const char source_identifier[] = "simulated0";

typedef enum simulated_state
{
    SIM_CREATED,
    SIM_OPEN,
    SIM_CONFIGURED,
    SIM_STARTED,
    SIM_STOPPED
} simulated_state;

typedef struct simulated_buffer
{
    uint8_t* storage;
    uint64_t token;
    bool outstanding;
} simulated_buffer;

struct camstream_camera_instance
{
    simulated_state state;
    simulated_buffer buffers[SIM_BUFFER_COUNT];
    char* last_error;
    uint64_t next_sequence;
    uint64_t next_token;
    size_t next_buffer;
};

static void clear_error(camstream_camera_instance* camera)
{
    free(camera->last_error);
    camera->last_error = NULL;
}

static camstream_camera_status_t set_error(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* message)
{
    if (camera != NULL)
    {
        const size_t length = strlen(message);
        if (length == SIZE_MAX)
        {
            return status;
        }
        char* copy = malloc(length + 1U);
        if (copy != NULL)
        {
            memcpy(copy, message, length + 1U);
            free(camera->last_error);
            camera->last_error = copy;
        }
    }
    return status;
}

static bool valid_header(uint32_t version, uint32_t size, size_t minimum)
{
    return version == CAMSTREAM_CAMERA_ABI_VERSION_V1 && size >= minimum;
}

static size_t bounded_length(const char* text, size_t maximum)
{
    size_t length = 0U;
    while (length < maximum && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

static void fill_supported_configuration(camstream_camera_stream_config_v1* configuration)
{
    memset(configuration, 0, sizeof(*configuration));
    configuration->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    configuration->struct_size = sizeof(*configuration);
    memcpy(configuration->source_identifier, source_identifier, sizeof(source_identifier));
    configuration->width = SIM_WIDTH;
    configuration->height = SIM_HEIGHT;
    configuration->pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    configuration->frame_rate_numerator = 30U;
    configuration->frame_rate_denominator = 1U;
}

static bool supported_configuration(const camstream_camera_stream_config_v1* configuration)
{
    if (!valid_header(configuration->abi_version, configuration->struct_size, sizeof(*configuration)))
    {
        return false;
    }
    const size_t length = bounded_length(configuration->source_identifier, CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE);
    return length == sizeof(source_identifier) - 1U &&
           memcmp(configuration->source_identifier, source_identifier, sizeof(source_identifier)) == 0 &&
           configuration->width == SIM_WIDTH && configuration->height == SIM_HEIGHT &&
           configuration->pixel_format == CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV &&
           configuration->frame_rate_numerator == 30U && configuration->frame_rate_denominator == 1U;
}

static bool has_outstanding_frame(const camstream_camera_instance* camera)
{
    for (size_t index = 0U; index < SIM_BUFFER_COUNT; ++index)
    {
        if (camera->buffers[index].outstanding)
        {
            return true;
        }
    }
    return false;
}

static bool has_available_buffer(const camstream_camera_instance* camera)
{
    for (size_t index = 0U; index < SIM_BUFFER_COUNT; ++index)
    {
        if (!camera->buffers[index].outstanding)
        {
            return true;
        }
    }
    return false;
}

static simulated_buffer* find_available_buffer(camstream_camera_instance* camera)
{
    for (size_t offset = 0U; offset < SIM_BUFFER_COUNT; ++offset)
    {
        const size_t index = (camera->next_buffer + offset) % SIM_BUFFER_COUNT;
        if (!camera->buffers[index].outstanding)
        {
            camera->next_buffer = (index + 1U) % SIM_BUFFER_COUNT;
            return &camera->buffers[index];
        }
    }
    return NULL;
}

static simulated_buffer* find_token(camstream_camera_instance* camera, uint64_t token)
{
    for (size_t index = 0U; index < SIM_BUFFER_COUNT; ++index)
    {
        if (camera->buffers[index].outstanding && camera->buffers[index].token == token)
        {
            return &camera->buffers[index];
        }
    }
    return NULL;
}

static camstream_camera_status_t simulated_create(camstream_camera_instance** instance)
{
    if (instance == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *instance = NULL;
    camstream_camera_instance* camera = calloc(1U, sizeof(*camera));
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    for (size_t index = 0U; index < SIM_BUFFER_COUNT; ++index)
    {
        camera->buffers[index].storage = malloc(SIM_FRAME_SIZE);
        if (camera->buffers[index].storage == NULL)
        {
            for (size_t prior = 0U; prior < index; ++prior)
            {
                free(camera->buffers[prior].storage);
            }
            free(camera);
            return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        }
    }
    camera->state = SIM_CREATED;
    camera->next_token = SIM_INITIAL_TOKEN;
    *instance = camera;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static void simulated_destroy(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return;
    }
    for (size_t index = 0U; index < SIM_BUFFER_COUNT; ++index)
    {
        free(camera->buffers[index].storage);
    }
    free(camera->last_error);
    free(camera);
}

static camstream_camera_status_t simulated_open(camstream_camera_instance* camera, const char* source)
{
    if (camera == NULL || source == NULL || source[0] == '\0')
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "open requires a source identifier");
    }
    if (camera->state != SIM_CREATED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "open requires Created state");
    }
    if (strcmp(source, source_identifier) != 0)
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "simulated backend supports only source simulated0");
    }
    camera->state = SIM_OPEN;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_close(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state == SIM_CREATED)
    {
        clear_error(camera);
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    if (camera->state == SIM_STARTED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires stop first");
    }
    if (has_outstanding_frame(camera))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires every frame to be released");
    }
    camera->state = SIM_CREATED;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_get_capabilities(
        camstream_camera_instance* camera, camstream_camera_capabilities_v1* capabilities)
{
    if (camera == NULL || capabilities == NULL ||
            !valid_header(capabilities->abi_version, capabilities->struct_size, sizeof(*capabilities)))
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "get_capabilities received invalid output storage");
    }
    if (camera->state != SIM_OPEN && camera->state != SIM_CONFIGURED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "get_capabilities requires an open source");
    }
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    capabilities->struct_size = sizeof(*capabilities);
    capabilities->stream_config_count = 1U;
    capabilities->maximum_plane_count = 1U;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_get_stream_configuration(
        camstream_camera_instance* camera, uint32_t index, camstream_camera_stream_config_v1* configuration)
{
    if (camera == NULL || configuration == NULL ||
            !valid_header(configuration->abi_version, configuration->struct_size, sizeof(*configuration)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT,
                "get_stream_configuration received invalid output storage");
    }
    if (camera->state != SIM_OPEN)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "get_stream_configuration requires Open state");
    }
    if (index != 0U)
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "stream-configuration index is out of range");
    }
    fill_supported_configuration(configuration);
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_configure(camstream_camera_instance* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    if (camera == NULL || requested == NULL || active == NULL ||
            !valid_header(active->abi_version, active->struct_size, sizeof(*active)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "configure received invalid arguments");
    }
    if (camera->state != SIM_OPEN)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "configure requires Open state");
    }
    if (!supported_configuration(requested))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED,
                "simulated backend supports only simulated0 64x48 YUYV at 30/1");
    }
    fill_supported_configuration(active);
    camera->state = SIM_CONFIGURED;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_start(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state != SIM_CONFIGURED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "start requires Configured state");
    }
    camera->next_sequence = 0U;
    camera->next_token = SIM_INITIAL_TOKEN;
    camera->next_buffer = 0U;
    for (size_t index = 0U; index < SIM_BUFFER_COUNT; ++index)
    {
        camera->buffers[index].token = 0U;
        camera->buffers[index].outstanding = false;
    }
    camera->state = SIM_STARTED;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_wait_frame(camstream_camera_instance* camera, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state != SIM_STARTED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "wait_frame requires Started state");
    }
    if (camera->next_token == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "simulated frame-token space is exhausted");
    }
    if (!has_available_buffer(camera))
    {
        return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
    }
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_acquire_frame(
        camstream_camera_instance* camera, camstream_camera_frame_v1* frame)
{
    if (camera == NULL || frame == NULL || !valid_header(frame->abi_version, frame->struct_size, sizeof(*frame)))
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "acquire_frame received invalid output storage");
    }
    if (camera->state != SIM_STARTED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "acquire_frame requires Started state");
    }
    if (camera->next_token == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "simulated frame-token space is exhausted");
    }
    simulated_buffer* buffer = find_available_buffer(camera);
    if (buffer == NULL)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "all simulated frame buffers are outstanding");
    }

    const uint64_t sequence = camera->next_sequence;
    for (size_t index = 0U; index < SIM_FRAME_SIZE; ++index)
    {
        buffer->storage[index] = (uint8_t)((sequence + (uint64_t)index) & UINT64_C(0xff));
    }
    struct timespec timestamp = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "simulated monotonic clock failed");
    }

    ++camera->next_sequence;
    buffer->token = camera->next_token++;
    buffer->outstanding = true;

    memset(frame, 0, sizeof(*frame));
    frame->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->struct_size = sizeof(*frame);
    frame->frame_token = buffer->token;
    frame->sequence_number = sequence;
    frame->monotonic_timestamp_ns = (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) + (uint64_t)timestamp.tv_nsec;
    frame->width = SIM_WIDTH;
    frame->height = SIM_HEIGHT;
    frame->pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    frame->plane_count = 1U;
    frame->planes[0].abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->planes[0].struct_size = sizeof(frame->planes[0]);
    frame->planes[0].data = buffer->storage;
    frame->planes[0].allocation_size = SIM_FRAME_SIZE;
    frame->planes[0].bytes_used = SIM_FRAME_SIZE;
    frame->planes[0].stride = SIM_STRIDE;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_release_frame(camstream_camera_instance* camera, uint64_t token)
{
    if (camera == NULL || token == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "release_frame requires a nonzero token");
    }
    if (camera->state != SIM_STARTED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "release_frame requires Started state");
    }
    simulated_buffer* buffer = find_token(camera, token);
    if (buffer == NULL)
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "frame token is unknown or already released");
    }
    buffer->outstanding = false;
    buffer->token = 0U;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_stop(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state == SIM_STOPPED)
    {
        clear_error(camera);
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    if (camera->state != SIM_STARTED)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires Started state");
    }
    if (has_outstanding_frame(camera))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires every frame to be released");
    }
    camera->state = SIM_STOPPED;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t simulated_get_last_error(
        camstream_camera_instance* camera, char* buffer, uint32_t buffer_size)
{
    if (camera == NULL || buffer == NULL || buffer_size == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    const int result = snprintf(buffer, buffer_size, "%s", camera->last_error != NULL ? camera->last_error : "");
    if (result < 0)
    {
        buffer[0] = '\0';
        return CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

__attribute__((constructor)) static void inject_simulated_backend(void)
{
    camstream_camera_binding_v2* binding = NULL;
    if (camstream_camera_hal_get_binding_slot_v2(&binding, sizeof(camstream_camera_binding_v2),
                sizeof(camstream_camera_ops_v2)) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return;
    }

    *binding->control_funcs = (camstream_camera_ops_v2){
            .struct_size = sizeof(camstream_camera_ops_v2),
            .camera_create = simulated_create,
            .camera_destroy = simulated_destroy,
            .camera_open = simulated_open,
            .camera_close = simulated_close,
            .camera_get_capabilities = simulated_get_capabilities,
            .camera_get_stream_configuration = simulated_get_stream_configuration,
            .camera_configure = simulated_configure,
            .camera_start = simulated_start,
            .camera_wait_frame = simulated_wait_frame,
            .camera_acquire_frame = simulated_acquire_frame,
            .camera_release_frame = simulated_release_frame,
            .camera_stop = simulated_stop,
            .camera_get_last_error = simulated_get_last_error,
    };
    binding->backend_name = backend_name;
    binding->struct_size = sizeof(*binding);
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
}
