#include <camstream/camera/camera_hal.h>

#include "camera_backend_runtime.h"

#include <camstream/camera/camera_backend.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum camera_state
{
    CAMERA_CREATED,
    CAMERA_OPEN,
    CAMERA_CONFIGURED,
    CAMERA_STARTED,
    CAMERA_STOPPED
} camera_state;

typedef struct outstanding_frame
{
    uint64_t public_token;
    uint64_t backend_token;
} outstanding_frame;

struct camstream_camera
{
    const camstream_camera_backend_v1* operations;
    camstream_camera_instance* instance;
    camera_state state;
    outstanding_frame* frames;
    size_t frame_count;
    size_t frame_capacity;
    uint64_t pending_cleanup_token;
    char* hal_error;
    bool ownership_faulted;
};

/* Never reset this counter on instance destruction or module reload. Zero means permanently exhausted. */
static _Atomic uint64_t next_public_frame_token = UINT64_C(1);

static size_t bounded_length(const char* text, size_t maximum)
{
    if (text == NULL)
    {
        return maximum;
    }
    size_t length = 0U;
    while (length < maximum && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

static bool valid_header(uint32_t version, uint32_t size, size_t minimum)
{
    return version == CAMSTREAM_CAMERA_ABI_VERSION_V1 && size >= minimum;
}

static bool valid_source_identifier(const char* source)
{
    const size_t length = bounded_length(source, CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE);
    return length > 0U && length < CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE;
}

static bool valid_configuration(const camstream_camera_stream_config_v1* config)
{
    return valid_header(config->abi_version, config->struct_size, sizeof(*config)) &&
           valid_source_identifier(config->source_identifier) && config->width > 0U && config->height > 0U &&
           config->pixel_format != 0U && config->frame_rate_numerator > 0U && config->frame_rate_denominator > 0U;
}

static bool valid_capabilities(const camstream_camera_capabilities_v1* caps)
{
    return valid_header(caps->abi_version, caps->struct_size, sizeof(*caps)) && caps->stream_config_count > 0U &&
           caps->maximum_plane_count > 0U && caps->maximum_plane_count <= CAMSTREAM_CAMERA_MAX_PLANES;
}

static bool valid_frame(const camstream_camera_frame_v1* frame)
{
    if (!valid_header(frame->abi_version, frame->struct_size, sizeof(*frame)) || frame->frame_token == 0U ||
            frame->width == 0U || frame->height == 0U || frame->pixel_format == 0U || frame->plane_count == 0U ||
            frame->plane_count > CAMSTREAM_CAMERA_MAX_PLANES)
    {
        return false;
    }
    for (uint32_t index = 0U; index < frame->plane_count; ++index)
    {
        const camstream_camera_plane_v1* plane = &frame->planes[index];
        if (!valid_header(plane->abi_version, plane->struct_size, sizeof(*plane)) || plane->data == NULL ||
                plane->allocation_size == 0U || plane->bytes_used == 0U || plane->bytes_used > plane->allocation_size ||
                plane->stride == 0U)
        {
            return false;
        }
    }
    return true;
}

static void init_configuration(camstream_camera_stream_config_v1* config)
{
    memset(config, 0, sizeof(*config));
    config->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    config->struct_size = sizeof(*config);
}

static void init_capabilities(camstream_camera_capabilities_v1* caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    caps->struct_size = sizeof(*caps);
}

static void init_frame(camstream_camera_frame_v1* frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    frame->struct_size = sizeof(*frame);
    for (uint32_t index = 0U; index < CAMSTREAM_CAMERA_MAX_PLANES; ++index)
    {
        frame->planes[index].abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
        frame->planes[index].struct_size = sizeof(frame->planes[index]);
    }
}

static uint64_t allocate_public_token(void)
{
    uint64_t candidate = atomic_load_explicit(&next_public_frame_token, memory_order_relaxed);
    while (candidate != 0U)
    {
        const uint64_t following = candidate == UINT64_MAX ? 0U : candidate + 1U;
        if (atomic_compare_exchange_weak_explicit(
                    &next_public_frame_token, &candidate, following, memory_order_relaxed, memory_order_relaxed))
        {
            return candidate;
        }
    }
    return 0U;
}

static camstream_camera_status_t set_hal_error(
        camstream_camera* camera, camstream_camera_status_t status, const char* message)
{
    if (camera != NULL)
    {
        const char* source = message != NULL ? message : "Camera HAL operation failed";
        const size_t length = strlen(source);
        if (length < SIZE_MAX)
        {
            char* copy = malloc(length + 1U);
            if (copy != NULL)
            {
                memcpy(copy, source, length + 1U);
                free(camera->hal_error);
                camera->hal_error = copy;
            }
        }
    }
    return status;
}

static void clear_hal_error(camstream_camera* camera)
{
    if (camera != NULL)
    {
        free(camera->hal_error);
        camera->hal_error = NULL;
    }
}

static camstream_camera_status_t release_backend_token(camstream_camera* camera, uint64_t backend_token)
{
    return camera->operations->release_frame(camera->instance, backend_token);
}

static camstream_camera_status_t rollback_acquired_frame(
        camstream_camera* camera, uint64_t backend_token, camstream_camera_status_t status, const char* error)
{
    const camstream_camera_status_t release_status = release_backend_token(camera, backend_token);
    if (release_status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        return set_hal_error(camera, status, error);
    }
    camera->pending_cleanup_token = backend_token;
    return set_hal_error(
            camera, release_status, "acquired-frame validation failed and rollback release_frame also failed");
}

static camstream_camera_status_t retry_pending_cleanup(camstream_camera* camera)
{
    if (camera->pending_cleanup_token == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    const camstream_camera_status_t status = release_backend_token(camera, camera->pending_cleanup_token);
    if (status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        camera->pending_cleanup_token = 0U;
    }
    return status;
}

static void cleanup_camera(camstream_camera* camera)
{
    if (camera->pending_cleanup_token != 0U)
    {
        (void)release_backend_token(camera, camera->pending_cleanup_token);
        camera->pending_cleanup_token = 0U;
    }
    for (size_t index = camera->frame_count; index > 0U; --index)
    {
        (void)release_backend_token(camera, camera->frames[index - 1U].backend_token);
    }
    camera->frame_count = 0U;
    if (camera->state == CAMERA_STARTED && camera->operations->stop(camera->instance) == CAMSTREAM_CAMERA_STATUS_OK)
    {
        camera->state = CAMERA_STOPPED;
    }
    if (camera->state == CAMERA_OPEN || camera->state == CAMERA_CONFIGURED || camera->state == CAMERA_STOPPED)
    {
        (void)camera->operations->close(camera->instance);
    }
}

camstream_camera_status_t camstream_camera_hal_load_backend(const char* backend_path)
{
    return camstream_runtime_load_backend(backend_path);
}

camstream_camera_status_t camstream_camera_hal_unload_backend(void)
{
    return camstream_runtime_unload_backend();
}

camstream_camera_status_t camstream_camera_hal_get_last_error(char* buffer, uint32_t buffer_size)
{
    return camstream_runtime_copy_error(buffer, buffer_size);
}

camstream_camera_status_t camstream_camera_create(camstream_camera** camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *camera = NULL;

    const camstream_camera_backend_v1* operations = NULL;
    camstream_camera_status_t status = camstream_runtime_reserve_create(&operations);
    camstream_camera_status_t cancel_status;
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }

    camstream_camera_instance* instance = NULL;
    camstream_camera* created = NULL;
    status = operations->create(&instance);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        camstream_runtime_set_error("backend create failed without an instance diagnostic");
        if (instance != NULL)
        {
            char diagnostic[256] = {0};
            if (operations->get_last_error(instance, diagnostic, sizeof(diagnostic)) == CAMSTREAM_CAMERA_STATUS_OK)
            {
                camstream_runtime_set_error(diagnostic);
            }
        }
        goto cleanup_partial;
    }
    if (instance == NULL)
    {
        camstream_runtime_set_error("backend create returned success with a null instance");
        status = CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
        goto cancel_reservation;
    }

    created = calloc(1U, sizeof(*created));
    if (created == NULL)
    {
        camstream_runtime_set_error("Camera HAL camera allocation failed");
        status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        goto cleanup_partial;
    }
    created->operations = operations;
    created->instance = instance;
    created->state = CAMERA_CREATED;

    status = camstream_runtime_commit_create();
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        goto cleanup_partial;
    }
    *camera = created;
    return CAMSTREAM_CAMERA_STATUS_OK;

cleanup_partial:
    /* Keep the create reservation until a partial backend instance is gone. */
    if (instance != NULL)
    {
        operations->destroy(instance);
    }
    free(created);

cancel_reservation:
    cancel_status = camstream_runtime_cancel_create();
    return cancel_status == CAMSTREAM_CAMERA_STATUS_OK ? status : cancel_status;
}

void camstream_camera_destroy(camstream_camera* camera)
{
    if (camera == NULL)
    {
        return;
    }
    cleanup_camera(camera);
    camera->operations->destroy(camera->instance);
    free(camera->frames);
    free(camera->hal_error);
    free(camera);
    camstream_runtime_record_destroy();
}

camstream_camera_status_t camstream_camera_get_backend_name(
        camstream_camera* camera, char* buffer, uint32_t buffer_size)
{
    if (camera == NULL || buffer == NULL || buffer_size == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    const char* name = camera->operations->backend_name;
    const size_t length = strlen(name);
    if (length >= buffer_size)
    {
        buffer[0] = '\0';
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "backend-name buffer is too small");
    }
    memcpy(buffer, name, length + 1U);
    clear_hal_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t camstream_camera_get_backend_abi_version(camstream_camera* camera, uint32_t* abi_version)
{
    if (camera == NULL || abi_version == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *abi_version = camera->operations->abi_version;
    clear_hal_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t camstream_camera_open(camstream_camera* camera, const char* source_identifier)
{
    if (camera == NULL || !valid_source_identifier(source_identifier))
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "open requires a bounded source");
    }
    if (camera->state != CAMERA_CREATED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "open requires Created state");
    }
    clear_hal_error(camera);
    const camstream_camera_status_t status = camera->operations->open(camera->instance, source_identifier);
    if (status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        camera->state = CAMERA_OPEN;
    }
    return status;
}

camstream_camera_status_t camstream_camera_close(camstream_camera* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state == CAMERA_CREATED)
    {
        clear_hal_error(camera);
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    if (camera->state == CAMERA_STARTED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires stop first");
    }
    if (camera->state != CAMERA_OPEN && camera->state != CAMERA_CONFIGURED && camera->state != CAMERA_STOPPED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires an open source");
    }
    if (camera->pending_cleanup_token != 0U || camera->frame_count != 0U)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires every frame released");
    }
    clear_hal_error(camera);
    const camstream_camera_status_t status = camera->operations->close(camera->instance);
    if (status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        camera->state = CAMERA_CREATED;
    }
    return status;
}

camstream_camera_status_t camstream_camera_get_capabilities(
        camstream_camera* camera, camstream_camera_capabilities_v1* capabilities)
{
    if (camera == NULL || capabilities == NULL ||
            !valid_header(capabilities->abi_version, capabilities->struct_size, sizeof(*capabilities)))
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "invalid capabilities output storage");
    }
    if (camera->state != CAMERA_OPEN && camera->state != CAMERA_CONFIGURED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "capabilities require an open source");
    }
    camstream_camera_capabilities_v1 result;
    init_capabilities(&result);
    clear_hal_error(camera);
    const camstream_camera_status_t status = camera->operations->get_capabilities(camera->instance, &result);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }
    if (!valid_capabilities(&result))
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "backend returned invalid capabilities");
    }
    *capabilities = result;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t camstream_camera_get_stream_configuration(
        camstream_camera* camera, uint32_t index, camstream_camera_stream_config_v1* configuration)
{
    if (camera == NULL || configuration == NULL ||
            !valid_header(configuration->abi_version, configuration->struct_size, sizeof(*configuration)))
    {
        return set_hal_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "invalid stream-configuration output storage");
    }
    if (camera->state != CAMERA_OPEN)
    {
        return set_hal_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stream-configuration discovery requires Open state");
    }
    camstream_camera_stream_config_v1 result;
    init_configuration(&result);
    clear_hal_error(camera);
    const camstream_camera_status_t status =
            camera->operations->get_stream_configuration(camera->instance, index, &result);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }
    if (!valid_configuration(&result))
    {
        return set_hal_error(
                camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "backend returned an invalid stream configuration");
    }
    *configuration = result;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t camstream_camera_configure(camstream_camera* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    if (camera == NULL || requested == NULL || active == NULL || !valid_configuration(requested) ||
            !valid_header(active->abi_version, active->struct_size, sizeof(*active)))
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "configure received invalid arguments");
    }
    if (camera->state != CAMERA_OPEN)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "configure requires Open state");
    }
    camstream_camera_stream_config_v1 result;
    init_configuration(&result);
    clear_hal_error(camera);
    const camstream_camera_status_t status = camera->operations->configure(camera->instance, requested, &result);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }
    if (!valid_configuration(&result))
    {
        return set_hal_error(
                camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "backend returned an invalid active configuration");
    }
    *active = result;
    camera->state = CAMERA_CONFIGURED;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t camstream_camera_start(camstream_camera* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state != CAMERA_CONFIGURED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "start requires Configured state");
    }
    clear_hal_error(camera);
    const camstream_camera_status_t status = camera->operations->start(camera->instance);
    if (status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        camera->state = CAMERA_STARTED;
    }
    return status;
}

camstream_camera_status_t camstream_camera_wait_frame(camstream_camera* camera, uint32_t timeout_ms)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state != CAMERA_STARTED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "wait_frame requires Started state");
    }
    if (camera->pending_cleanup_token != 0U || camera->ownership_faulted)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "frame ownership cleanup is unresolved");
    }
    clear_hal_error(camera);
    return camera->operations->wait_frame(camera->instance, timeout_ms);
}

camstream_camera_status_t camstream_camera_acquire_frame(camstream_camera* camera, camstream_camera_frame_v1* frame)
{
    if (camera == NULL || frame == NULL || !valid_header(frame->abi_version, frame->struct_size, sizeof(*frame)))
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "invalid frame output storage");
    }
    if (camera->state != CAMERA_STARTED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "acquire_frame requires Started state");
    }
    if (camera->pending_cleanup_token != 0U || camera->ownership_faulted)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "frame ownership cleanup is unresolved");
    }

    camstream_camera_frame_v1 acquired;
    init_frame(&acquired);
    clear_hal_error(camera);
    const camstream_camera_status_t status = camera->operations->acquire_frame(camera->instance, &acquired);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }

    if (!valid_frame(&acquired))
    {
        if (acquired.frame_token == 0U)
        {
            camera->ownership_faulted = true;
            return set_hal_error(
                    camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "backend returned invalid acquired-frame metadata");
        }
        return rollback_acquired_frame(camera, acquired.frame_token, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR,
                "backend returned invalid acquired-frame metadata");
    }

    for (size_t index = 0U; index < camera->frame_count; ++index)
    {
        if (camera->frames[index].backend_token == acquired.frame_token)
        {
            camera->ownership_faulted = true;
            return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR,
                    "backend returned a duplicate outstanding frame token");
        }
    }

    const uint64_t backend_token = acquired.frame_token;
    const uint64_t public_token = allocate_public_token();
    if (public_token == 0U)
    {
        return rollback_acquired_frame(camera, backend_token, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
                "Camera HAL frame-token space is exhausted");
    }

    if (camera->frame_count == camera->frame_capacity)
    {
        if (camera->frame_capacity > SIZE_MAX / 2U)
        {
            return rollback_acquired_frame(camera, backend_token, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
                    "Camera HAL frame tracking allocation failed");
        }
        const size_t new_capacity = camera->frame_capacity == 0U ? 1U : camera->frame_capacity * 2U;
        if (new_capacity > SIZE_MAX / sizeof(*camera->frames))
        {
            return rollback_acquired_frame(camera, backend_token, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
                    "Camera HAL frame tracking allocation failed");
        }
        void* grown = realloc(camera->frames, new_capacity * sizeof(*camera->frames));
        if (grown == NULL)
        {
            return rollback_acquired_frame(camera, backend_token, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
                    "Camera HAL frame tracking allocation failed");
        }
        camera->frames = grown;
        camera->frame_capacity = new_capacity;
    }

    camera->frames[camera->frame_count].public_token = public_token;
    camera->frames[camera->frame_count].backend_token = backend_token;
    ++camera->frame_count;

    acquired.frame_token = public_token;
    *frame = acquired;
    clear_hal_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t camstream_camera_release_frame(camstream_camera* camera, uint64_t frame_token)
{
    if (camera == NULL || frame_token == 0U)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "release_frame requires a token");
    }
    if (camera->state != CAMERA_STARTED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "release_frame requires Started state");
    }
    size_t index = 0U;
    while (index < camera->frame_count && camera->frames[index].public_token != frame_token)
    {
        ++index;
    }
    if (index == camera->frame_count)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT,
                "frame token is unknown, stale, or camera-owned elsewhere");
    }
    clear_hal_error(camera);
    const camstream_camera_status_t status = release_backend_token(camera, camera->frames[index].backend_token);
    if (status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        if (index + 1U < camera->frame_count)
        {
            memmove(&camera->frames[index], &camera->frames[index + 1U],
                    (camera->frame_count - index - 1U) * sizeof(*camera->frames));
        }
        --camera->frame_count;
    }
    return status;
}

camstream_camera_status_t camstream_camera_stop(camstream_camera* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->state == CAMERA_STOPPED)
    {
        clear_hal_error(camera);
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    if (camera->state != CAMERA_STARTED)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires Started state");
    }
    clear_hal_error(camera);
    const camstream_camera_status_t cleanup_status = retry_pending_cleanup(camera);
    if (cleanup_status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return cleanup_status;
    }
    if (camera->frame_count != 0U)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires every frame released");
    }
    if (camera->ownership_faulted)
    {
        return set_hal_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "frame ownership state is faulted");
    }
    const camstream_camera_status_t status = camera->operations->stop(camera->instance);
    if (status == CAMSTREAM_CAMERA_STATUS_OK)
    {
        camera->state = CAMERA_STOPPED;
    }
    return status;
}

camstream_camera_status_t camstream_camera_get_last_error(camstream_camera* camera, char* buffer, uint32_t buffer_size)
{
    if (camera == NULL || buffer == NULL || buffer_size == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->hal_error != NULL && camera->hal_error[0] != '\0')
    {
        const size_t maximum = (size_t)buffer_size - 1U;
        size_t length = strlen(camera->hal_error);
        if (length > maximum)
        {
            length = maximum;
        }
        memcpy(buffer, camera->hal_error, length);
        buffer[length] = '\0';
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    return camera->operations->get_last_error(camera->instance, buffer, buffer_size);
}
