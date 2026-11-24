#define _POSIX_C_SOURCE 200809L
#include "v4l2_internal.h"

#define CAMSTREAM_LOG_MODULE "v4l2"
#ifdef CAMSTREAM_V4L2_DEBUG_LOG
#define CAMSTREAM_ENABLE_DEBUG_LOG
#endif
#include <camstream/common/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef CAMSTREAM_V4L2_TEST_IO
extern int camstream_test_v4l2_open(const char* path, int flags);
extern int camstream_test_v4l2_ioctl(int fd, unsigned long request, void* argument);
extern int camstream_test_v4l2_close(int fd);
extern void* camstream_test_v4l2_mmap(size_t length, int protection, int flags, int fd, off_t offset);
extern int camstream_test_v4l2_munmap(void* address, size_t length);
extern int camstream_test_v4l2_poll(struct pollfd* descriptor, int timeout_ms);
#endif

int v4l2_open_device(const char* path)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
#else
    return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
#endif
}

int v4l2_ioctl_device(int fd, unsigned long request, void* argument)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_ioctl(fd, request, argument);
#else
    return ioctl(fd, request, argument);
#endif
}

int v4l2_close_device(int fd)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_close(fd);
#else
    return close(fd);
#endif
}

int v4l2_poll_device(struct pollfd* descriptor, int timeout_ms)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_poll(descriptor, timeout_ms);
#else
    return poll(descriptor, 1U, timeout_ms);
#endif
}

void* v4l2_mmap_device(size_t length, int fd, off_t offset)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_mmap(length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
#else
    return mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
#endif
}

int v4l2_munmap_device(void* address, size_t length)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_munmap(address, length);
#else
    return munmap(address, length);
#endif
}

size_t v4l2_bounded_length(const char* text, size_t maximum)
{
    size_t length = 0U;
    while (length < maximum && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

bool v4l2_valid_header(uint32_t abi_version, uint32_t struct_size, size_t minimum_size)
{
    return abi_version == CAMSTREAM_CAMERA_ABI_VERSION_V1 && struct_size >= minimum_size;
}

static camstream_camera_status_t set_error_parts(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* prefix, const char* suffix)
{
    if (camera == NULL)
    {
        return status;
    }

    const size_t prefix_length = strlen(prefix);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length == SIZE_MAX || prefix_length > SIZE_MAX - suffix_length - 1U)
    {
        return status;
    }
    const size_t length = prefix_length + suffix_length;
    char* message = malloc(length + 1U);
    if (message == NULL)
    {
        return status;
    }
    memcpy(message, prefix, prefix_length);
    memcpy(message + prefix_length, suffix, suffix_length + 1U);
    free(camera->last_error);
    camera->last_error = message;
    return status;
}

camstream_camera_status_t v4l2_set_error(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* message)
{
    return set_error_parts(camera, status, message, "");
}

camstream_camera_status_t v4l2_set_errno_status(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* operation, int error_number)
{
    const char* reason = strerror(error_number);
    if (reason == NULL)
    {
        reason = "unknown error";
    }
    CAM_LOGE("%s: errno=%d (%s)", operation, error_number, reason);

    const size_t operation_length = strlen(operation);
    const size_t reason_length = strlen(reason);
    static const char separator[] = " failed: ";
    if (operation_length > SIZE_MAX - sizeof(separator) ||
            reason_length > SIZE_MAX - operation_length - sizeof(separator))
    {
        return status;
    }
    const size_t length = operation_length + sizeof(separator) - 1U + reason_length;
    char* message = malloc(length + 1U);
    if (message == NULL)
    {
        return status;
    }
    memcpy(message, operation, operation_length);
    memcpy(message + operation_length, separator, sizeof(separator) - 1U);
    memcpy(message + operation_length + sizeof(separator) - 1U, reason, reason_length + 1U);
    free(camera->last_error);
    camera->last_error = message;
    return status;
}

camstream_camera_status_t v4l2_set_errno_error(
        camstream_camera_instance* camera, const char* operation, int error_number)
{
    return v4l2_set_errno_status(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, operation, error_number);
}

void v4l2_append_errno_detail(
        camstream_camera_instance* camera, const char* phase, const char* operation, int error_number)
{
    char detail[192];
    const char* reason = strerror(error_number);
    if (reason == NULL)
    {
        reason = "unknown error";
    }
    const int written = snprintf(detail, sizeof(detail), "; %s %s failed: %s", phase, operation, reason);
    if (written > 0)
    {
        const char* original = camera->last_error != NULL ? camera->last_error : "V4L2 cleanup failed";
        (void)set_error_parts(camera, CAMSTREAM_CAMERA_STATUS_OK, original, detail);
    }
}

void v4l2_clear_error(camstream_camera_instance* camera)
{
    free(camera->last_error);
    camera->last_error = NULL;
}

static void reset_open_state(camstream_camera_instance* camera)
{
    camera->fd = -1;
    camera->source_identifier[0] = '\0';
    free(camera->configurations.items);
    camera->configurations = (configuration_list){0};
    camera->active_configuration = (camstream_camera_stream_config_v1){0};
    camera->allocated_buffer_count = 0U;
    camera->buffer_release_required = false;
    free(camera->mapped_buffers);
    camera->mapped_buffers = NULL;
    camera->mapped_buffer_count = 0U;
    camera->active_stride = 0U;
    camera->configured = false;
    camera->reopen_required = false;
    camera->streaming = false;
    camera->streamoff_required = false;
}

camstream_camera_status_t v4l2_create(camstream_camera_instance** instance)
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
    camera->fd = -1;
    camera->next_frame_token = 1U;
    *instance = camera;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

void v4l2_destroy(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return;
    }
    if (camera->fd >= 0)
    {
        bool descriptor_closed = false;
        if (camera->streamoff_required && v4l2_streamoff_device(camera->fd) == -1)
        {
            const int error_number = errno;
            CAM_LOGW("destroy VIDIOC_STREAMOFF: errno=%d (%s)", error_number, strerror(error_number));
            if (v4l2_close_device(camera->fd) == -1)
            {
                const int close_error = errno;
                CAM_LOGW("destroy close: errno=%d (%s)", close_error, strerror(close_error));
            }
            descriptor_closed = true;
        }
        const bool unmapped = v4l2_unmap_buffers(camera);
        if (!descriptor_closed && unmapped && camera->buffer_release_required &&
                v4l2_release_requested_buffers(camera->fd) == -1)
        {
            const int error_number = errno;
            CAM_LOGW("destroy VIDIOC_REQBUFS(count=0): errno=%d (%s)", error_number, strerror(error_number));
        }
        if (!descriptor_closed && v4l2_close_device(camera->fd) == -1)
        {
            const int error_number = errno;
            CAM_LOGW("destroy close: errno=%d (%s)", error_number, strerror(error_number));
        }
    }
    reset_open_state(camera);
    v4l2_clear_error(camera);
    free(camera);
}

camstream_camera_status_t v4l2_open(camstream_camera_instance* camera, const char* source_identifier)
{
    if (camera == NULL || source_identifier == NULL || source_identifier[0] == '\0')
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "open requires a source identifier");
    }
    if (camera->fd >= 0)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "open requires a closed V4L2 instance");
    }
    const size_t source_length = v4l2_bounded_length(source_identifier, CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE);
    if (source_length == 0U || source_length >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "source identifier is not bounded");
    }

    const int fd = v4l2_open_device(source_identifier);
    if (fd == -1)
    {
        const int error_number = errno;
        return v4l2_set_errno_error(camera, "open", error_number);
    }

    configuration_list configurations = {0};
    camstream_camera_status_t status;
    struct v4l2_capability capability = {0};
    if (v4l2_ioctl_device(fd, VIDIOC_QUERYCAP, &capability) == -1)
    {
        const int error_number = errno;
        status = v4l2_set_errno_error(camera, "VIDIOC_QUERYCAP", error_number);
        goto failed;
    }

    const uint32_t effective_caps =
            (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U ? capability.device_caps : capability.capabilities;
    if ((effective_caps & V4L2_CAP_VIDEO_CAPTURE) == 0U)
    {
        status = v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "device does not support single-planar video capture");
        goto failed;
    }
    if ((effective_caps & V4L2_CAP_STREAMING) == 0U)
    {
        status = v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "device does not support streaming I/O");
        goto failed;
    }

    status = v4l2_enumerate_configurations(camera, fd, &configurations);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        goto failed;
    }
    memcpy(camera->source_identifier, source_identifier, source_length + 1U);
    camera->configurations = configurations;
    camera->fd = fd;
    v4l2_clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;

failed:
    free(configurations.items);
    (void)v4l2_close_device(fd);
    return status;
}

camstream_camera_status_t v4l2_close(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->fd < 0)
    {
        reset_open_state(camera);
        v4l2_clear_error(camera);
        return CAMSTREAM_CAMERA_STATUS_OK;
    }

    if (camera->streaming)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires Stop and released frames");
    }
    if (camera->streamoff_required)
    {
        if (v4l2_streamoff_device(camera->fd) == -1)
        {
            const int error_number = errno;
            return v4l2_set_errno_error(camera, "VIDIOC_STREAMOFF during close", error_number);
        }
        camera->streamoff_required = false;
    }

    if (!v4l2_unmap_buffers(camera))
    {
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    /* STREAMOFF or unmap failures retain the FD for cleanup. Once close is called, Linux may reuse
     * its number even if close reports an error, so record warnings and reset ownership.
     */
    bool cleanup_warning = false;
    if (camera->buffer_release_required && v4l2_release_requested_buffers(camera->fd) == -1)
    {
        const int error_number = errno;
        (void)v4l2_set_errno_error(camera, "VIDIOC_REQBUFS(count=0)", error_number);
        cleanup_warning = true;
    }
    if (v4l2_close_device(camera->fd) == -1)
    {
        const int error_number = errno;
        if (cleanup_warning)
        {
            v4l2_append_errno_detail(camera, "cleanup", "close", error_number);
        }
        else
        {
            (void)v4l2_set_errno_error(camera, "close", error_number);
        }
        cleanup_warning = true;
    }
    reset_open_state(camera);
    if (!cleanup_warning)
    {
        v4l2_clear_error(camera);
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t v4l2_get_last_error(camstream_camera_instance* camera, char* buffer, uint32_t buffer_size)
{
    if (camera == NULL || buffer == NULL || buffer_size == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    const char* error = camera->last_error != NULL ? camera->last_error : "";
    size_t length = strlen(error);
    if (length >= buffer_size)
    {
        length = buffer_size - 1U;
    }
    memcpy(buffer, error, length);
    buffer[length] = '\0';
    return CAMSTREAM_CAMERA_STATUS_OK;
}
