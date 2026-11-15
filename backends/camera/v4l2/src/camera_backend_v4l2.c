#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_backend.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifdef CAMSTREAM_V4L2_TEST_IO
extern int camstream_test_v4l2_open(const char* path, int flags);
extern int camstream_test_v4l2_ioctl(int fd, unsigned long request, void* argument);
extern int camstream_test_v4l2_close(int fd);
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

struct camstream_camera_instance
{
    int fd;
    char source_identifier[CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE];
    configuration_list configurations;
    char* last_error;
};

static const char backend_name[] = "v4l2";

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

static int retry_ioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do
    {
        result = ioctl_device(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
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

static camstream_camera_status_t set_errno_error(
        camstream_camera_instance* camera, const char* operation, int error_number)
{
    const char* reason = strerror(error_number);
    if (reason == NULL)
    {
        reason = "unknown error";
    }

    const size_t operation_length = strlen(operation);
    const size_t reason_length = strlen(reason);
    static const char separator[] = " failed: ";
    if (operation_length > SIZE_MAX - sizeof(separator) ||
            reason_length > SIZE_MAX - operation_length - sizeof(separator))
    {
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    const size_t length = operation_length + sizeof(separator) - 1U + reason_length;
    char* message = malloc(length + 1U);
    if (message == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    memcpy(message, operation, operation_length);
    memcpy(message + operation_length, separator, sizeof(separator) - 1U);
    memcpy(message + operation_length + sizeof(separator) - 1U, reason, reason_length + 1U);
    free(camera->last_error);
    camera->last_error = message;
    return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
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
        if (retry_ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1)
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
        if (retry_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1)
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
        if (retry_ioctl(fd, VIDIOC_ENUM_FMT, &format) == -1)
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

static camstream_camera_status_t unsupported_operation(camstream_camera_instance* camera, const char* operation)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (camera->fd < 0)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "operation requires an open V4L2 device");
    }
    return set_error_parts(
            camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, operation, " is not implemented in Stage 8.5 Phase 1");
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
        (void)close_device(camera->fd);
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
    if (retry_ioctl(fd, VIDIOC_QUERYCAP, &capability) == -1)
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

    const int fd = camera->fd;
    reset_open_state(camera);
    if (close_device(fd) == -1)
    {
        const int error_number = errno;
        return set_errno_error(camera, "close", error_number);
    }
    clear_error(camera);
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

static camstream_camera_status_t v4l2_configure(camstream_camera_instance* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    if (camera == NULL || requested == NULL || active == NULL ||
            !valid_header(requested->abi_version, requested->struct_size, sizeof(*requested)) ||
            !valid_header(active->abi_version, active->struct_size, sizeof(*active)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "configure received invalid arguments");
    }
    return unsupported_operation(camera, "configure");
}

static camstream_camera_status_t v4l2_start(camstream_camera_instance* camera)
{
    return unsupported_operation(camera, "start");
}

static camstream_camera_status_t v4l2_wait_frame(camstream_camera_instance* camera, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return unsupported_operation(camera, "wait_frame");
}

static camstream_camera_status_t v4l2_acquire_frame(camstream_camera_instance* camera, camstream_camera_frame_v1* frame)
{
    if (camera == NULL || frame == NULL || !valid_header(frame->abi_version, frame->struct_size, sizeof(*frame)))
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "acquire_frame received invalid output");
    }
    return unsupported_operation(camera, "acquire_frame");
}

static camstream_camera_status_t v4l2_release_frame(camstream_camera_instance* camera, uint64_t frame_token)
{
    if (camera == NULL || frame_token == 0U)
    {
        return set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "release_frame requires a nonzero token");
    }
    return unsupported_operation(camera, "release_frame");
}

static camstream_camera_status_t v4l2_stop(camstream_camera_instance* camera)
{
    return unsupported_operation(camera, "stop");
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
