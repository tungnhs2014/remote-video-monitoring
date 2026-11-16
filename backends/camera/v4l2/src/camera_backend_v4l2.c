#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_backend.h>
#define CAMSTREAM_LOG_MODULE "v4l2"
#ifdef CAMSTREAM_V4L2_DEBUG_LOG
#define CAMSTREAM_ENABLE_DEBUG_LOG
#endif
#include <camstream/common/log.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef CAMSTREAM_V4L2_TEST_IO
extern int camstream_test_v4l2_open(const char* path, int flags);
extern int camstream_test_v4l2_ioctl(int fd, unsigned long request, void* argument);
extern int camstream_test_v4l2_close(int fd);
extern void* camstream_test_v4l2_mmap(size_t length, int protection, int flags, int fd, off_t offset);
extern int camstream_test_v4l2_munmap(void* address, size_t length);
extern int camstream_test_v4l2_poll(struct pollfd* descriptor, int timeout_ms);
#endif

typedef struct v4l2_stream_configuration
{
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t frame_rate_numerator;
    uint32_t frame_rate_denominator;
} v4l2_stream_configuration;

typedef struct configuration_list
{
    v4l2_stream_configuration* items;
    size_t count;
    size_t capacity;
} configuration_list;

typedef struct mapped_buffer
{
    void* address;
    size_t length;
    uint64_t frame_token;
    bool queued;
    bool outstanding;
} mapped_buffer;

struct camstream_camera_instance
{
    int fd;
    char source_identifier[CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE];
    configuration_list configurations;
    char* last_error;
    camstream_camera_stream_config_v1 active_configuration;
    uint32_t allocated_buffer_count;
    bool buffer_release_required;
    mapped_buffer* mapped_buffers;
    uint32_t mapped_buffer_count;
    uint32_t active_stride;
    uint64_t next_frame_token;
    bool configured;
    bool reopen_required;
    bool streaming;
    bool streamoff_required;
};

static const char backend_name[] = "v4l2";
static const uint32_t requested_buffer_count = 4U;

static int open_device(const char* path)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
#else
    return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
#endif
}

static int ioctl_device(int fd, unsigned long request, void* argument)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_ioctl(fd, request, argument);
#else
    return ioctl(fd, request, argument);
#endif
}

static int close_device(int fd)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_close(fd);
#else
    return close(fd);
#endif
}

static int poll_device(struct pollfd* descriptor, int timeout_ms)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_poll(descriptor, timeout_ms);
#else
    return poll(descriptor, 1U, timeout_ms);
#endif
}

static void* mmap_device(size_t length, int fd, off_t offset)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_mmap(length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
#else
    return mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
#endif
}

static int munmap_device(void* address, size_t length)
{
#ifdef CAMSTREAM_V4L2_TEST_IO
    return camstream_test_v4l2_munmap(address, length);
#else
    return munmap(address, length);
#endif
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

static bool valid_header(uint32_t abi_version, uint32_t struct_size, size_t minimum_size)
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

static camstream_camera_status_t set_error(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* message)
{
    return set_error_parts(camera, status, message, "");
}

static camstream_camera_status_t set_errno_status(
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

static camstream_camera_status_t set_errno_error(
        camstream_camera_instance* camera, const char* operation, int error_number)
{
    return set_errno_status(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, operation, error_number);
}

static void append_errno_detail(
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

/**
 * @brief Releases this descriptor's MMAP queue after userspace mappings are removed.
 */
static int release_requested_buffers(int fd)
{
    struct v4l2_requestbuffers release = {0};
    release.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    release.memory = V4L2_MEMORY_MMAP;
    return ioctl_device(fd, VIDIOC_REQBUFS, &release);
}

static int streamoff_device(int fd)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl_device(fd, VIDIOC_STREAMOFF, &type);
}

/** @return True only when every userspace mapping has been removed. Failed mappings remain owned for Close retry. */
static bool unmap_buffers(camstream_camera_instance* camera)
{
    bool complete = true;
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        mapped_buffer* buffer = &camera->mapped_buffers[index];
        if (buffer->length != 0U)
        {
            if (munmap_device(buffer->address, buffer->length) == -1)
            {
                const int error_number = errno;
                CAM_LOGW("munmap index=%u: errno=%d (%s)", index, error_number, strerror(error_number));
                append_errno_detail(camera, "cleanup", "munmap", error_number);
                complete = false;
            }
            else
            {
                buffer->address = NULL;
                buffer->length = 0U;
            }
        }
    }
    if (complete)
    {
        free(camera->mapped_buffers);
        camera->mapped_buffers = NULL;
        camera->mapped_buffer_count = 0U;
    }
    return complete;
}

static void clear_error(camstream_camera_instance* camera)
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

static bool configurations_match(const v4l2_stream_configuration* left, const v4l2_stream_configuration* right)
{
    return left->width == right->width && left->height == right->height && left->pixel_format == right->pixel_format &&
           left->frame_rate_numerator == right->frame_rate_numerator &&
           left->frame_rate_denominator == right->frame_rate_denominator;
}

static camstream_camera_status_t append_configuration(
        camstream_camera_instance* camera, configuration_list* list, const v4l2_stream_configuration* configuration)
{
    for (size_t index = 0U; index < list->count; ++index)
    {
        if (configurations_match(&list->items[index], configuration))
        {
            return CAMSTREAM_CAMERA_STATUS_OK;
        }
    }
    if (list->count == UINT32_MAX)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
                "V4L2 stream-configuration count exceeds Camera HAL v1");
    }
    if (list->count == list->capacity)
    {
        if (list->capacity > SIZE_MAX / 2U)
        {
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 configuration allocation overflow");
        }
        const size_t capacity = list->capacity == 0U ? 4U : list->capacity * 2U;
        if (capacity > SIZE_MAX / sizeof(*list->items))
        {
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 configuration allocation overflow");
        }
        void* grown = realloc(list->items, capacity * sizeof(*list->items));
        if (grown == NULL)
        {
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 configuration allocation failed");
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = *configuration;
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t enumerate_intervals(camstream_camera_instance* camera, int fd, uint32_t pixel_format,
        uint32_t width, uint32_t height, configuration_list* list)
{
    for (uint32_t index = 0U;; ++index)
    {
        struct v4l2_frmivalenum interval = {0};
        interval.index = index;
        interval.pixel_format = pixel_format;
        interval.width = width;
        interval.height = height;
        if (ioctl_device(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1)
        {
            const int error_number = errno;
            if (error_number == EINVAL)
            {
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
            return set_errno_error(camera, "VIDIOC_ENUM_FRAMEINTERVALS", error_number);
        }

        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE && interval.discrete.numerator != 0U &&
                interval.discrete.denominator != 0U)
        {
            const v4l2_stream_configuration configuration = {
                    .width = width,
                    .height = height,
                    .pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV,
                    .frame_rate_numerator = interval.discrete.denominator,
                    .frame_rate_denominator = interval.discrete.numerator,
            };
            const camstream_camera_status_t status = append_configuration(camera, list, &configuration);
            if (status != CAMSTREAM_CAMERA_STATUS_OK)
            {
                return status;
            }
        }
        if (index == UINT32_MAX)
        {
            return set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 frame-interval enumeration index overflow");
        }
    }
}

static camstream_camera_status_t enumerate_sizes(
        camstream_camera_instance* camera, int fd, uint32_t pixel_format, configuration_list* list)
{
    for (uint32_t index = 0U;; ++index)
    {
        struct v4l2_frmsizeenum size = {0};
        size.index = index;
        size.pixel_format = pixel_format;
        if (ioctl_device(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1)
        {
            const int error_number = errno;
            if (error_number == EINVAL)
            {
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
            return set_errno_error(camera, "VIDIOC_ENUM_FRAMESIZES", error_number);
        }

        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE && size.discrete.width != 0U && size.discrete.height != 0U)
        {
            const camstream_camera_status_t status =
                    enumerate_intervals(camera, fd, pixel_format, size.discrete.width, size.discrete.height, list);
            if (status != CAMSTREAM_CAMERA_STATUS_OK)
            {
                return status;
            }
        }
        if (index == UINT32_MAX)
        {
            return set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 frame-size enumeration index overflow");
        }
    }
}

static camstream_camera_status_t enumerate_configurations(
        camstream_camera_instance* camera, int fd, configuration_list* list)
{
    for (uint32_t index = 0U;; ++index)
    {
        struct v4l2_fmtdesc format = {0};
        format.index = index;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl_device(fd, VIDIOC_ENUM_FMT, &format) == -1)
        {
            const int error_number = errno;
            if (error_number == EINVAL)
            {
                break;
            }
            return set_errno_error(camera, "VIDIOC_ENUM_FMT", error_number);
        }

        if (format.pixelformat == V4L2_PIX_FMT_YUYV)
        {
            const camstream_camera_status_t status = enumerate_sizes(camera, fd, format.pixelformat, list);
            if (status != CAMSTREAM_CAMERA_STATUS_OK)
            {
                return status;
            }
        }
        if (index == UINT32_MAX)
        {
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 format enumeration index overflow");
        }
    }

    if (list->count == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED,
                "device exposes no discrete YUYV size and frame-interval combinations representable by Camera HAL v1");
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t v4l2_create(camstream_camera_instance** instance)
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

static void v4l2_destroy(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return;
    }
    if (camera->fd >= 0)
    {
        bool descriptor_closed = false;
        if (camera->streamoff_required && streamoff_device(camera->fd) == -1)
        {
            const int error_number = errno;
            CAM_LOGW("destroy VIDIOC_STREAMOFF: errno=%d (%s)", error_number, strerror(error_number));
            if (close_device(camera->fd) == -1)
            {
                const int close_error = errno;
                CAM_LOGW("destroy close: errno=%d (%s)", close_error, strerror(close_error));
            }
            descriptor_closed = true;
        }
        const bool unmapped = unmap_buffers(camera);
        if (!descriptor_closed && unmapped && camera->buffer_release_required &&
                release_requested_buffers(camera->fd) == -1)
        {
            const int error_number = errno;
            CAM_LOGW("destroy VIDIOC_REQBUFS(count=0): errno=%d (%s)", error_number, strerror(error_number));
        }
        if (!descriptor_closed && close_device(camera->fd) == -1)
        {
            const int error_number = errno;
            CAM_LOGW("destroy close: errno=%d (%s)", error_number, strerror(error_number));
        }
    }
    reset_open_state(camera);
    clear_error(camera);
    free(camera);
}

static camstream_camera_status_t v4l2_open(camstream_camera_instance* camera, const char* source_identifier)
{
    if (camera == NULL || source_identifier == NULL || source_identifier[0] == '\0')
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "open requires a source identifier");
    }
    if (camera->fd >= 0)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "open requires a closed V4L2 instance");
    }
    const size_t source_length = bounded_length(source_identifier, CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE);
    if (source_length == 0U || source_length >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "source identifier is not bounded");
    }

    const int fd = open_device(source_identifier);
    if (fd == -1)
    {
        const int error_number = errno;
        return set_errno_error(camera, "open", error_number);
    }

    configuration_list configurations = {0};
    camstream_camera_status_t status;
    struct v4l2_capability capability = {0};
    if (ioctl_device(fd, VIDIOC_QUERYCAP, &capability) == -1)
    {
        const int error_number = errno;
        status = set_errno_error(camera, "VIDIOC_QUERYCAP", error_number);
        goto failed;
    }

    const uint32_t effective_caps =
            (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U ? capability.device_caps : capability.capabilities;
    if ((effective_caps & V4L2_CAP_VIDEO_CAPTURE) == 0U)
    {
        status = set_error(
                camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "device does not support single-planar video capture");
        goto failed;
    }
    if ((effective_caps & V4L2_CAP_STREAMING) == 0U)
    {
        status = set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "device does not support streaming I/O");
        goto failed;
    }

    status = enumerate_configurations(camera, fd, &configurations);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        goto failed;
    }
    memcpy(camera->source_identifier, source_identifier, source_length + 1U);
    camera->configurations = configurations;
    camera->fd = fd;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;

failed:
    free(configurations.items);
    (void)close_device(fd);
    return status;
}

static camstream_camera_status_t v4l2_close(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->fd < 0)
    {
        reset_open_state(camera);
        clear_error(camera);
        return CAMSTREAM_CAMERA_STATUS_OK;
    }

    if (camera->streaming)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "close requires Stop and released frames");
    }
    if (camera->streamoff_required)
    {
        if (streamoff_device(camera->fd) == -1)
        {
            const int error_number = errno;
            return set_errno_error(camera, "VIDIOC_STREAMOFF during close", error_number);
        }
        camera->streamoff_required = false;
    }

    if (!unmap_buffers(camera))
    {
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    bool cleanup_warning = false;
    if (camera->buffer_release_required && release_requested_buffers(camera->fd) == -1)
    {
        const int error_number = errno;
        (void)set_errno_error(camera, "VIDIOC_REQBUFS(count=0)", error_number);
        cleanup_warning = true;
    }
    if (close_device(camera->fd) == -1)
    {
        const int error_number = errno;
        if (cleanup_warning)
        {
            append_errno_detail(camera, "cleanup", "close", error_number);
        }
        else
        {
            (void)set_errno_error(camera, "close", error_number);
        }
        cleanup_warning = true;
    }
    reset_open_state(camera);
    if (!cleanup_warning)
    {
        clear_error(camera);
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t v4l2_get_capabilities(
        camstream_camera_instance* camera, camstream_camera_capabilities_v1* capabilities)
{
    if (camera == NULL || capabilities == NULL ||
            !valid_header(capabilities->abi_version, capabilities->struct_size, sizeof(*capabilities)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "get_capabilities received invalid output");
    }
    if (camera->fd < 0)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "get_capabilities requires an open device");
    }
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    capabilities->struct_size = sizeof(*capabilities);
    capabilities->stream_config_count = (uint32_t)camera->configurations.count;
    capabilities->maximum_plane_count = 1U;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t v4l2_get_stream_configuration(
        camstream_camera_instance* camera, uint32_t index, camstream_camera_stream_config_v1* configuration)
{
    if (camera == NULL || configuration == NULL ||
            !valid_header(configuration->abi_version, configuration->struct_size, sizeof(*configuration)))
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "get_stream_configuration received invalid output");
    }
    if (camera->fd < 0)
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "get_stream_configuration requires an open device");
    }
    if ((size_t)index >= camera->configurations.count)
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "stream-configuration index is out of range");
    }

    const v4l2_stream_configuration* selected = &camera->configurations.items[index];
    memset(configuration, 0, sizeof(*configuration));
    configuration->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    configuration->struct_size = sizeof(*configuration);
    memcpy(configuration->source_identifier, camera->source_identifier, strlen(camera->source_identifier) + 1U);
    configuration->width = selected->width;
    configuration->height = selected->height;
    configuration->pixel_format = selected->pixel_format;
    configuration->frame_rate_numerator = selected->frame_rate_numerator;
    configuration->frame_rate_denominator = selected->frame_rate_denominator;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static bool supported_request(
        const camstream_camera_instance* camera, const camstream_camera_stream_config_v1* requested)
{
    for (size_t index = 0U; index < camera->configurations.count; ++index)
    {
        const v4l2_stream_configuration* supported = &camera->configurations.items[index];
        if (supported->width == requested->width && supported->height == requested->height &&
                supported->pixel_format == requested->pixel_format &&
                (uint64_t)supported->frame_rate_numerator * requested->frame_rate_denominator ==
                        (uint64_t)requested->frame_rate_numerator * supported->frame_rate_denominator)
        {
            return true;
        }
    }
    return false;
}

static camstream_camera_status_t validate_request(
        camstream_camera_instance* camera, const camstream_camera_stream_config_v1* requested)
{
    const size_t source_length = bounded_length(requested->source_identifier, CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE);
    if (source_length == 0U || source_length >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE ||
            strcmp(requested->source_identifier, camera->source_identifier) != 0 || requested->width == 0U ||
            requested->height == 0U || requested->frame_rate_numerator == 0U ||
            requested->frame_rate_denominator == 0U || requested->width > UINT32_MAX / 2U ||
            (requested->width & 1U) != 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "invalid V4L2 configuration request");
    }
    if (requested->pixel_format != CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "only YUYV is supported by Camera HAL v1");
    }
    if (!supported_request(camera, requested))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "configuration was not discovered for source");
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t validate_active_format(
        camstream_camera_instance* camera, const struct v4l2_format* format)
{
    if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || format->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
    {
        return set_error(
                camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "driver selected a format unsupported by Camera HAL v1");
    }

    const uint32_t width = format->fmt.pix.width;
    const uint32_t height = format->fmt.pix.height;
    const uint32_t stride = format->fmt.pix.bytesperline;
    const uint32_t image_size = format->fmt.pix.sizeimage;
    if (width == 0U || height == 0U || (width & 1U) != 0U || width > UINT32_MAX / 2U ||
            format->fmt.pix.field != V4L2_FIELD_NONE || stride < width * 2U || (uint64_t)stride * height > UINT32_MAX ||
            image_size < (uint64_t)stride * height)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR,
                "driver returned an invalid or unrepresentable YUYV buffer layout");
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @brief Keeps the original Configure failure and releases only resources owned by this instance. */
static camstream_camera_status_t fail_configuration(
        camstream_camera_instance* camera, camstream_camera_status_t failure)
{
    camera->reopen_required = true;
    if (!unmap_buffers(camera))
    {
        return failure;
    }
    if (camera->buffer_release_required && release_requested_buffers(camera->fd) == -1)
    {
        const int error_number = errno;
        CAM_LOGW("cleanup VIDIOC_REQBUFS(count=0): errno=%d (%s)", error_number, strerror(error_number));
        append_errno_detail(camera, "cleanup", "VIDIOC_REQBUFS(count=0)", error_number);
    }
    else
    {
        camera->allocated_buffer_count = 0U;
        camera->buffer_release_required = false;
    }
    return failure;
}

/** @brief Queries, maps and initially queues the actual driver allocation; failure retains cleanup ownership. */
static camstream_camera_status_t prepare_buffers(camstream_camera_instance* camera, uint32_t image_size)
{
    const uint32_t count = camera->allocated_buffer_count;
    if ((uint64_t)count * sizeof(*camera->mapped_buffers) > SIZE_MAX)
    {
        CAM_LOGE("MMAP metadata size overflow: count=%u", count);
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "MMAP buffer metadata size overflow");
    }
    camera->mapped_buffers = calloc(count, sizeof(*camera->mapped_buffers));
    if (camera->mapped_buffers == NULL)
    {
        CAM_LOGE("MMAP metadata allocation failed: count=%u", count);
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "MMAP buffer metadata allocation failed");
    }

    for (uint32_t index = 0U; index < count; ++index)
    {
        struct v4l2_buffer buffer = {0};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl_device(camera->fd, VIDIOC_QUERYBUF, &buffer) == -1)
        {
            const int error_number = errno;
            return set_errno_error(camera, "VIDIOC_QUERYBUF", error_number);
        }
        const off_t offset = (off_t)buffer.m.offset;
        if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffer.memory != V4L2_MEMORY_MMAP || buffer.index != index ||
                buffer.length < image_size || buffer.length == 0U || offset < 0 || (uint64_t)offset != buffer.m.offset)
        {
            CAM_LOGE("invalid QUERYBUF metadata: index=%u returned=%u type=%u memory=%u length=%u", index, buffer.index,
                    buffer.type, buffer.memory, buffer.length);
            return set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid QUERYBUF metadata");
        }

        void* address = mmap_device(buffer.length, camera->fd, offset);
        if (address == MAP_FAILED)
        {
            const int error_number = errno;
            return set_errno_error(camera, "mmap", error_number);
        }
        camera->mapped_buffers[index] = (mapped_buffer){.address = address, .length = buffer.length};
        ++camera->mapped_buffer_count;
        if (address == NULL)
        {
            CAM_LOGE("mmap returned a NULL address for index=%u", index);
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR,
                    "mmap returned a NULL address that Camera HAL v1 cannot publish");
        }
        CAM_LOGD("mapped index=%u length=%u", index, buffer.length);
    }

    for (uint32_t index = 0U; index < count; ++index)
    {
        struct v4l2_buffer buffer = {0};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl_device(camera->fd, VIDIOC_QBUF, &buffer) == -1)
        {
            const int error_number = errno;
            return set_errno_error(camera, "VIDIOC_QBUF", error_number);
        }
        camera->mapped_buffers[index].queued = true;
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/**
 * @brief Negotiates one advertised YUYV mode and prepares its MMAP queue.
 * @note The caller serializes this instance. A failure after S_FMT requires Close/Reopen; no active output is
 * published.
 */
static camstream_camera_status_t v4l2_configure(camstream_camera_instance* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    if (camera == NULL || requested == NULL || active == NULL ||
            !valid_header(requested->abi_version, requested->struct_size, sizeof(*requested)) ||
            !valid_header(active->abi_version, active->struct_size, sizeof(*active)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "configure received invalid arguments");
    }
    if (camera->fd < 0 || camera->configured || camera->reopen_required)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE,
                "configure requires an open, unconfigured device; close and reopen after failure");
    }
    camstream_camera_status_t status = validate_request(camera, requested);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }

    struct v4l2_format previous_format = {0};
    previous_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_device(camera->fd, VIDIOC_G_FMT, &previous_format) == -1)
    {
        const int error_number = errno;
        return set_errno_error(camera, "VIDIOC_G_FMT", error_number);
    }
    if (previous_format.type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned an invalid current format");
    }

    struct v4l2_streamparm previous_interval = {0};
    previous_interval.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_device(camera->fd, VIDIOC_G_PARM, &previous_interval) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status = (error_number == EINVAL || error_number == ENOTTY)
                                                               ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED
                                                               : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        return set_errno_status(camera, error_status, "VIDIOC_G_PARM", error_number);
    }
    if (previous_interval.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (previous_interval.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0U ||
            previous_interval.parm.capture.timeperframe.numerator == 0U ||
            previous_interval.parm.capture.timeperframe.denominator == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED,
                "driver cannot confirm a valid configurable frame interval");
    }

    struct v4l2_format negotiated_format = {0};
    negotiated_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    negotiated_format.fmt.pix.width = requested->width;
    negotiated_format.fmt.pix.height = requested->height;
    negotiated_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    negotiated_format.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl_device(camera->fd, VIDIOC_S_FMT, &negotiated_format) == -1)
    {
        const int error_number = errno;
        status = set_errno_error(camera, "VIDIOC_S_FMT", error_number);
        return fail_configuration(camera, status);
    }
    status = validate_active_format(camera, &negotiated_format);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return fail_configuration(camera, status);
    }

    struct v4l2_streamparm requested_interval = previous_interval;
    requested_interval.parm.capture.timeperframe.numerator = requested->frame_rate_denominator;
    requested_interval.parm.capture.timeperframe.denominator = requested->frame_rate_numerator;
    if (ioctl_device(camera->fd, VIDIOC_S_PARM, &requested_interval) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status = (error_number == EINVAL || error_number == ENOTTY)
                                                               ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED
                                                               : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        status = set_errno_status(camera, error_status, "VIDIOC_S_PARM", error_number);
        return fail_configuration(camera, status);
    }

    struct v4l2_streamparm confirmed_interval = {0};
    confirmed_interval.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_device(camera->fd, VIDIOC_G_PARM, &confirmed_interval) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status = (error_number == EINVAL || error_number == ENOTTY)
                                                               ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED
                                                               : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        status = set_errno_status(camera, error_status, "VIDIOC_G_PARM", error_number);
        return fail_configuration(camera, status);
    }
    if (confirmed_interval.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (confirmed_interval.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0U ||
            confirmed_interval.parm.capture.timeperframe.numerator == 0U ||
            confirmed_interval.parm.capture.timeperframe.denominator == 0U)
    {
        status = set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned an invalid confirmed frame interval");
        return fail_configuration(camera, status);
    }

    struct v4l2_requestbuffers buffers = {0};
    buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffers.memory = V4L2_MEMORY_MMAP;
    buffers.count = requested_buffer_count;
    if (ioctl_device(camera->fd, VIDIOC_REQBUFS, &buffers) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status =
                error_number == EINVAL ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        status = set_errno_status(camera, error_status, "VIDIOC_REQBUFS", error_number);
        if (error_number != EINVAL && error_number != EBUSY)
        {
            camera->buffer_release_required = true; /* Allocation may have partially succeeded. */
        }
        return fail_configuration(camera, status);
    }
    if (buffers.type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffers.memory != V4L2_MEMORY_MMAP)
    {
        CAM_LOGE("invalid REQBUFS metadata: type=%u memory=%u count=%u", buffers.type, buffers.memory, buffers.count);
        camera->allocated_buffer_count = buffers.count;
        camera->buffer_release_required = buffers.count > 0U;
        status = set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned an invalid buffer queue");
        return fail_configuration(camera, status);
    }
    if (buffers.count == 0U)
    {
        CAM_LOGE("VIDIOC_REQBUFS returned zero buffers");
        status = set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "driver allocated no MMAP buffers");
        return fail_configuration(camera, status);
    }
    camera->allocated_buffer_count = buffers.count;
    camera->buffer_release_required = true;
    status = prepare_buffers(camera, negotiated_format.fmt.pix.sizeimage);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return fail_configuration(camera, status);
    }

    camstream_camera_stream_config_v1 result = {0};
    result.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    result.struct_size = sizeof(result);
    memcpy(result.source_identifier, camera->source_identifier, strlen(camera->source_identifier) + 1U);
    result.width = negotiated_format.fmt.pix.width;
    result.height = negotiated_format.fmt.pix.height;
    result.pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    result.frame_rate_numerator = confirmed_interval.parm.capture.timeperframe.denominator;
    result.frame_rate_denominator = confirmed_interval.parm.capture.timeperframe.numerator;

    camera->active_configuration = result;
    camera->active_stride = negotiated_format.fmt.pix.bytesperline;
    camera->configured = true;
    *active = result;
    CAM_LOGI("buffers ready source=%s requested=%u actual=%u mapped=%u queued=%u", camera->source_identifier,
            requested_buffer_count, buffers.count, camera->mapped_buffer_count, buffers.count);
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @return A checked CLOCK_MONOTONIC observation, used when the driver timestamp clock is unknown. */
static bool monotonic_now_ns(uint64_t* timestamp)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
    {
        return false;
    }
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000 ||
            (uint64_t)now.tv_sec > (UINT64_MAX - UINT64_C(999999999)) / UINT64_C(1000000000))
    {
        errno = ERANGE;
        return false;
    }
    *timestamp = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}

static bool frame_timestamp_ns(const struct v4l2_buffer* buffer, uint64_t* timestamp)
{
    if ((buffer->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
            buffer->timestamp.tv_sec >= 0 && buffer->timestamp.tv_usec >= 0 && buffer->timestamp.tv_usec < 1000000 &&
            (uint64_t)buffer->timestamp.tv_sec <= (UINT64_MAX - UINT64_C(999999999)) / UINT64_C(1000000000))
    {
        *timestamp = (uint64_t)buffer->timestamp.tv_sec * UINT64_C(1000000000) +
                     (uint64_t)buffer->timestamp.tv_usec * UINT64_C(1000);
        return true;
    }
    return monotonic_now_ns(timestamp);
}

/** @brief Starts the already queued MMAP buffers; a failed attempt needs Close/Reopen. */
static camstream_camera_status_t v4l2_start(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!camera->configured || camera->streaming || camera->reopen_required ||
            camera->mapped_buffer_count != camera->allocated_buffer_count)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "start requires prepared buffers");
    }
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        if (!camera->mapped_buffers[index].queued || camera->mapped_buffers[index].outstanding)
        {
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "start requires every buffer queued");
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera->streamoff_required = true;
    if (ioctl_device(camera->fd, VIDIOC_STREAMON, &type) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t failure = set_errno_error(camera, "VIDIOC_STREAMON", error_number);
        camera->reopen_required = true;
        if (streamoff_device(camera->fd) == -1)
        {
            const int cleanup_error = errno;
            CAM_LOGW("cleanup VIDIOC_STREAMOFF: errno=%d (%s)", cleanup_error, strerror(cleanup_error));
            append_errno_detail(camera, "cleanup", "VIDIOC_STREAMOFF", cleanup_error);
        }
        else
        {
            camera->streamoff_required = false;
        }
        return failure;
    }
    camera->streaming = true;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @brief Waits only until the caller's deadline; EINTR retries use the remaining monotonic budget. */
static camstream_camera_status_t v4l2_wait_frame(camstream_camera_instance* camera, uint32_t timeout_ms)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!camera->streaming || camera->reopen_required)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "wait_frame requires active streaming");
    }
    if (timeout_ms > INT_MAX)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "wait_frame timeout exceeds poll range");
    }

    uint64_t now;
    if (!monotonic_now_ns(&now))
    {
        return set_errno_error(camera, "clock_gettime", errno);
    }
    if (now > UINT64_MAX - (uint64_t)timeout_ms * UINT64_C(1000000))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "wait_frame deadline overflows");
    }
    const uint64_t deadline = now + (uint64_t)timeout_ms * UINT64_C(1000000);
    int remaining_ms = (int)timeout_ms;
    for (;;)
    {
        struct pollfd descriptor = {.fd = camera->fd, .events = POLLIN};
        const int result = poll_device(&descriptor, remaining_ms);
        if (result > 0)
        {
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                CAM_LOGE("poll device error: revents=0x%x", (unsigned)descriptor.revents);
                return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "poll reported device error");
            }
            if ((descriptor.revents & POLLIN) != 0)
            {
                clear_error(camera);
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
            CAM_LOGE("poll returned no capture readiness: revents=0x%x", (unsigned)descriptor.revents);
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "poll returned unexpected events");
        }
        if (result == 0)
        {
            return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
        }
        const int error_number = errno;
        if (error_number != EINTR)
        {
            return set_errno_error(camera, "poll", error_number);
        }
        if (!monotonic_now_ns(&now))
        {
            return set_errno_error(camera, "clock_gettime", errno);
        }
        if (now >= deadline)
        {
            return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
        }
        remaining_ms = (int)((deadline - now + UINT64_C(999999)) / UINT64_C(1000000));
    }
}

/** @brief Dequeues one checked single-planar image and assigns a private per-instance token. */
static camstream_camera_status_t v4l2_acquire_frame(camstream_camera_instance* camera, camstream_camera_frame_v1* frame)
{
    if (camera == NULL || frame == NULL || !valid_header(frame->abi_version, frame->struct_size, sizeof(*frame)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "acquire_frame received invalid output");
    }
    if (!camera->streaming || camera->reopen_required)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "acquire_frame requires active streaming");
    }
    if (camera->next_frame_token == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "private frame token space is exhausted");
    }

    struct v4l2_buffer buffer = {0};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl_device(camera->fd, VIDIOC_DQBUF, &buffer) == -1)
    {
        const int error_number = errno;
        if (error_number == EAGAIN)
        {
            return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
        }
        return set_errno_error(camera, "VIDIOC_DQBUF", error_number);
    }
    if (buffer.index >= camera->mapped_buffer_count)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF returned invalid index=%u", buffer.index);
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid buffer index");
    }

    mapped_buffer* mapped = &camera->mapped_buffers[buffer.index];
    if (!mapped->queued || mapped->outstanding || mapped->length == 0U)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF returned buffer without queue ownership: index=%u", buffer.index);
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned unowned buffer");
    }
    mapped->queued = false;
    if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffer.memory != V4L2_MEMORY_MMAP)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF invalid type or memory: index=%u type=%u memory=%u", buffer.index, buffer.type,
                buffer.memory);
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid frame metadata");
    }
    if ((buffer.flags & V4L2_BUF_FLAG_ERROR) != 0U)
    {
        CAM_LOGW(
                "VIDIOC_DQBUF reported a recoverable frame error: index=%u sequence=%u", buffer.index, buffer.sequence);
        struct v4l2_buffer requeue = {0};
        requeue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        requeue.memory = V4L2_MEMORY_MMAP;
        requeue.index = buffer.index;
        if (ioctl_device(camera->fd, VIDIOC_QBUF, &requeue) == -1)
        {
            const int error_number = errno;
            camera->reopen_required = true;
            return set_errno_error(camera, "VIDIOC_QBUF after frame error", error_number);
        }
        mapped->queued = true;
        clear_error(camera);
        return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
    }
    const uint64_t minimum_size = (uint64_t)camera->active_stride * camera->active_configuration.height;
    if (buffer.length == 0U || buffer.length > mapped->length || buffer.bytesused < minimum_size ||
            buffer.bytesused > buffer.length || buffer.field != V4L2_FIELD_NONE || mapped->address == NULL)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF invalid metadata: index=%u length=%u bytesused=%u flags=0x%x", buffer.index,
                buffer.length, buffer.bytesused, buffer.flags);
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid frame metadata");
    }

    uint64_t timestamp;
    if (!frame_timestamp_ns(&buffer, &timestamp))
    {
        const int error_number = errno;
        camera->reopen_required = true;
        return set_errno_error(camera, "clock_gettime", error_number);
    }
    camstream_camera_frame_v1 result = {0};
    result.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    result.struct_size = sizeof(result);
    result.frame_token = camera->next_frame_token;
    result.sequence_number = buffer.sequence;
    result.monotonic_timestamp_ns = timestamp;
    result.width = camera->active_configuration.width;
    result.height = camera->active_configuration.height;
    result.pixel_format = camera->active_configuration.pixel_format;
    result.plane_count = 1U;
    result.planes[0].abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    result.planes[0].struct_size = sizeof(result.planes[0]);
    result.planes[0].data = mapped->address;
    result.planes[0].allocation_size = mapped->length;
    result.planes[0].bytes_used = buffer.bytesused;
    result.planes[0].stride = camera->active_stride;

    mapped->frame_token = camera->next_frame_token;
    mapped->outstanding = true;
    camera->next_frame_token = camera->next_frame_token == UINT64_MAX ? 0U : camera->next_frame_token + 1U;
    *frame = result;
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @brief Requeues only a matching outstanding private token; QBUF failure retains borrower ownership. */
static camstream_camera_status_t v4l2_release_frame(camstream_camera_instance* camera, uint64_t frame_token)
{
    if (camera == NULL || frame_token == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "release_frame requires a nonzero token");
    }
    if (!camera->streaming)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "release_frame requires active streaming");
    }
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        mapped_buffer* mapped = &camera->mapped_buffers[index];
        if (mapped->outstanding && mapped->frame_token == frame_token)
        {
            struct v4l2_buffer buffer = {0};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (ioctl_device(camera->fd, VIDIOC_QBUF, &buffer) == -1)
            {
                const int error_number = errno;
                return set_errno_error(camera, "VIDIOC_QBUF", error_number);
            }
            mapped->outstanding = false;
            mapped->frame_token = 0U;
            mapped->queued = true;
            clear_error(camera);
            return CAMSTREAM_CAMERA_STATUS_OK;
        }
    }
    return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "frame token is unknown or stale");
}

/** @brief Stops only after all borrowed frames are returned; a failed STREAMOFF leaves streaming state for retry. */
static camstream_camera_status_t v4l2_stop(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!camera->streaming)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires active streaming");
    }
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        if (camera->mapped_buffers[index].outstanding)
        {
            return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires every frame released");
        }
    }
    if (streamoff_device(camera->fd) == -1)
    {
        const int error_number = errno;
        return set_errno_error(camera, "VIDIOC_STREAMOFF", error_number);
    }
    camera->streaming = false;
    camera->streamoff_required = false;
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        camera->mapped_buffers[index].queued = false;
    }
    clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t v4l2_get_last_error(
        camstream_camera_instance* camera, char* buffer, uint32_t buffer_size)
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

static const camstream_camera_backend_v1 backend_descriptor = {
        .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
        .struct_size = sizeof(camstream_camera_backend_v1),
        .backend_name = backend_name,
        .create = v4l2_create,
        .destroy = v4l2_destroy,
        .open = v4l2_open,
        .close = v4l2_close,
        .get_capabilities = v4l2_get_capabilities,
        .get_stream_configuration = v4l2_get_stream_configuration,
        .configure = v4l2_configure,
        .start = v4l2_start,
        .wait_frame = v4l2_wait_frame,
        .acquire_frame = v4l2_acquire_frame,
        .release_frame = v4l2_release_frame,
        .stop = v4l2_stop,
        .get_last_error = v4l2_get_last_error,
};

__attribute__((constructor)) static void register_v4l2_backend(void)
{
    (void)camstream_camera_hal_register_backend_v1(&backend_descriptor);
}
