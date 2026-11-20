#define _POSIX_C_SOURCE 200809L
#include "v4l2_internal.h"

#define CAMSTREAM_LOG_MODULE "v4l2"
#ifdef CAMSTREAM_V4L2_DEBUG_LOG
#define CAMSTREAM_ENABLE_DEBUG_LOG
#endif
#include <camstream/common/log.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
                "V4L2 stream-configuration count exceeds Camera HAL v1");
    }
    if (list->count == list->capacity)
    {
        if (list->capacity > SIZE_MAX / 2U)
        {
            return v4l2_set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 configuration allocation overflow");
        }
        const size_t capacity = list->capacity == 0U ? 4U : list->capacity * 2U;
        if (capacity > SIZE_MAX / sizeof(*list->items))
        {
            return v4l2_set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 configuration allocation overflow");
        }
        void* grown = realloc(list->items, capacity * sizeof(*list->items));
        if (grown == NULL)
        {
            return v4l2_set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 configuration allocation failed");
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
        if (v4l2_ioctl_device(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1)
        {
            const int error_number = errno;
            if (error_number == EINVAL)
            {
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
            return v4l2_set_errno_error(camera, "VIDIOC_ENUM_FRAMEINTERVALS", error_number);
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
            return v4l2_set_error(
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
        if (v4l2_ioctl_device(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1)
        {
            const int error_number = errno;
            if (error_number == EINVAL)
            {
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
            return v4l2_set_errno_error(camera, "VIDIOC_ENUM_FRAMESIZES", error_number);
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
            return v4l2_set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 frame-size enumeration index overflow");
        }
    }
}

camstream_camera_status_t v4l2_enumerate_configurations(
        camstream_camera_instance* camera, int fd, configuration_list* list)
{
    for (uint32_t index = 0U;; ++index)
    {
        struct v4l2_fmtdesc format = {0};
        format.index = index;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (v4l2_ioctl_device(fd, VIDIOC_ENUM_FMT, &format) == -1)
        {
            const int error_number = errno;
            if (error_number == EINVAL)
            {
                break;
            }
            return v4l2_set_errno_error(camera, "VIDIOC_ENUM_FMT", error_number);
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
            return v4l2_set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "V4L2 format enumeration index overflow");
        }
    }

    if (list->count == 0U)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED,
                "device exposes no discrete YUYV size and frame-interval combinations representable by Camera HAL v1");
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t v4l2_get_capabilities(
        camstream_camera_instance* camera, camstream_camera_capabilities_v1* capabilities)
{
    if (camera == NULL || capabilities == NULL ||
            !v4l2_valid_header(capabilities->abi_version, capabilities->struct_size, sizeof(*capabilities)))
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "get_capabilities received invalid output");
    }
    if (camera->fd < 0)
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "get_capabilities requires an open device");
    }
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    capabilities->struct_size = sizeof(*capabilities);
    capabilities->stream_config_count = (uint32_t)camera->configurations.count;
    capabilities->maximum_plane_count = 1U;
    v4l2_clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

camstream_camera_status_t v4l2_get_stream_configuration(
        camstream_camera_instance* camera, uint32_t index, camstream_camera_stream_config_v1* configuration)
{
    if (camera == NULL || configuration == NULL ||
            !v4l2_valid_header(configuration->abi_version, configuration->struct_size, sizeof(*configuration)))
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "get_stream_configuration received invalid output");
    }
    if (camera->fd < 0)
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "get_stream_configuration requires an open device");
    }
    if ((size_t)index >= camera->configurations.count)
    {
        return v4l2_set_error(
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
    v4l2_clear_error(camera);
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
    const size_t source_length = v4l2_bounded_length(requested->source_identifier, CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE);
    if (source_length == 0U || source_length >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE ||
            strcmp(requested->source_identifier, camera->source_identifier) != 0 || requested->width == 0U ||
            requested->height == 0U || requested->frame_rate_numerator == 0U ||
            requested->frame_rate_denominator == 0U || requested->width > UINT32_MAX / 2U ||
            (requested->width & 1U) != 0U)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "invalid V4L2 configuration request");
    }
    if (requested->pixel_format != CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "only YUYV is supported by Camera HAL v1");
    }
    if (!supported_request(camera, requested))
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, "configuration was not discovered for source");
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static camstream_camera_status_t validate_active_format(
        camstream_camera_instance* camera, const struct v4l2_format* format)
{
    if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || format->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
    {
        return v4l2_set_error(
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
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR,
                "driver returned an invalid or unrepresentable YUYV buffer layout");
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/**
 * @brief Negotiates one advertised YUYV mode and prepares its MMAP queue.
 * @note The caller serializes this instance. A failure after S_FMT requires Close/Reopen; no active output is
 * published.
 */
camstream_camera_status_t v4l2_configure(camstream_camera_instance* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    if (camera == NULL || requested == NULL || active == NULL ||
            !v4l2_valid_header(requested->abi_version, requested->struct_size, sizeof(*requested)) ||
            !v4l2_valid_header(active->abi_version, active->struct_size, sizeof(*active)))
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "configure received invalid arguments");
    }
    if (camera->fd < 0 || camera->configured || camera->reopen_required)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE,
                "configure requires an open, unconfigured device; close and reopen after failure");
    }
    camstream_camera_status_t status = validate_request(camera, requested);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
    }

    struct v4l2_format previous_format = {0};
    previous_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_G_FMT, &previous_format) == -1)
    {
        const int error_number = errno;
        return v4l2_set_errno_error(camera, "VIDIOC_G_FMT", error_number);
    }
    if (previous_format.type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned an invalid current format");
    }

    struct v4l2_streamparm previous_interval = {0};
    previous_interval.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_G_PARM, &previous_interval) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status = (error_number == EINVAL || error_number == ENOTTY)
                                                               ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED
                                                               : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        return v4l2_set_errno_status(camera, error_status, "VIDIOC_G_PARM", error_number);
    }
    if (previous_interval.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (previous_interval.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0U ||
            previous_interval.parm.capture.timeperframe.numerator == 0U ||
            previous_interval.parm.capture.timeperframe.denominator == 0U)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED,
                "driver cannot confirm a valid configurable frame interval");
    }

    struct v4l2_format negotiated_format = {0};
    negotiated_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    negotiated_format.fmt.pix.width = requested->width;
    negotiated_format.fmt.pix.height = requested->height;
    negotiated_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    negotiated_format.fmt.pix.field = V4L2_FIELD_ANY;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_S_FMT, &negotiated_format) == -1)
    {
        const int error_number = errno;
        status = v4l2_set_errno_error(camera, "VIDIOC_S_FMT", error_number);
        return v4l2_fail_configuration(camera, status);
    }
    status = validate_active_format(camera, &negotiated_format);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return v4l2_fail_configuration(camera, status);
    }

    struct v4l2_streamparm requested_interval = previous_interval;
    requested_interval.parm.capture.timeperframe.numerator = requested->frame_rate_denominator;
    requested_interval.parm.capture.timeperframe.denominator = requested->frame_rate_numerator;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_S_PARM, &requested_interval) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status = (error_number == EINVAL || error_number == ENOTTY)
                                                               ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED
                                                               : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        status = v4l2_set_errno_status(camera, error_status, "VIDIOC_S_PARM", error_number);
        return v4l2_fail_configuration(camera, status);
    }

    struct v4l2_streamparm confirmed_interval = {0};
    confirmed_interval.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_G_PARM, &confirmed_interval) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status = (error_number == EINVAL || error_number == ENOTTY)
                                                               ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED
                                                               : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        status = v4l2_set_errno_status(camera, error_status, "VIDIOC_G_PARM", error_number);
        return v4l2_fail_configuration(camera, status);
    }
    if (confirmed_interval.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (confirmed_interval.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0U ||
            confirmed_interval.parm.capture.timeperframe.numerator == 0U ||
            confirmed_interval.parm.capture.timeperframe.denominator == 0U)
    {
        status = v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned an invalid confirmed frame interval");
        return v4l2_fail_configuration(camera, status);
    }

    status = v4l2_allocate_buffers(camera, negotiated_format.fmt.pix.sizeimage);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return status;
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
            (uint32_t)V4L2_REQUESTED_BUFFER_COUNT, camera->allocated_buffer_count, camera->mapped_buffer_count,
            camera->allocated_buffer_count);
    v4l2_clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}
