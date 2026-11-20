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
#include <sys/mman.h>
#include <time.h>

/**
 * @brief Releases this descriptor's MMAP queue after userspace mappings are removed.
 */
int v4l2_release_requested_buffers(int fd)
{
    struct v4l2_requestbuffers release = {0};
    release.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    release.memory = V4L2_MEMORY_MMAP;
    return v4l2_ioctl_device(fd, VIDIOC_REQBUFS, &release);
}

int v4l2_streamoff_device(int fd)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return v4l2_ioctl_device(fd, VIDIOC_STREAMOFF, &type);
}

/** @return True only when every userspace mapping has been removed. Failed mappings remain owned for Close retry. */
bool v4l2_unmap_buffers(camstream_camera_instance* camera)
{
    bool complete = true;
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        mapped_buffer* buffer = &camera->mapped_buffers[index];
        if (buffer->length != 0U)
        {
            if (v4l2_munmap_device(buffer->address, buffer->length) == -1)
            {
                const int error_number = errno;
                CAM_LOGW("munmap index=%u: errno=%d (%s)", index, error_number, strerror(error_number));
                v4l2_append_errno_detail(camera, "cleanup", "munmap", error_number);
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

/** @brief Keeps the original Configure failure and releases only resources owned by this instance. */
camstream_camera_status_t v4l2_fail_configuration(camstream_camera_instance* camera, camstream_camera_status_t failure)
{
    camera->reopen_required = true;
    if (!v4l2_unmap_buffers(camera))
    {
        return failure;
    }
    if (camera->buffer_release_required && v4l2_release_requested_buffers(camera->fd) == -1)
    {
        const int error_number = errno;
        CAM_LOGW("cleanup VIDIOC_REQBUFS(count=0): errno=%d (%s)", error_number, strerror(error_number));
        v4l2_append_errno_detail(camera, "cleanup", "VIDIOC_REQBUFS(count=0)", error_number);
    }
    else
    {
        camera->allocated_buffer_count = 0U;
        camera->buffer_release_required = false;
    }
    return failure;
}

/** @brief Queries, maps and initially queues the actual driver allocation; failure retains cleanup ownership. */
static camstream_camera_status_t v4l2_prepare_buffers(camstream_camera_instance* camera, uint32_t image_size)
{
    const uint32_t count = camera->allocated_buffer_count;
    if ((uint64_t)count * sizeof(*camera->mapped_buffers) > SIZE_MAX)
    {
        CAM_LOGE("MMAP metadata size overflow: count=%u", count);
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "MMAP buffer metadata size overflow");
    }
    camera->mapped_buffers = calloc(count, sizeof(*camera->mapped_buffers));
    if (camera->mapped_buffers == NULL)
    {
        CAM_LOGE("MMAP metadata allocation failed: count=%u", count);
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "MMAP buffer metadata allocation failed");
    }

    for (uint32_t index = 0U; index < count; ++index)
    {
        struct v4l2_buffer buffer = {0};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (v4l2_ioctl_device(camera->fd, VIDIOC_QUERYBUF, &buffer) == -1)
        {
            const int error_number = errno;
            return v4l2_set_errno_error(camera, "VIDIOC_QUERYBUF", error_number);
        }
        const off_t offset = (off_t)buffer.m.offset;
        if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffer.memory != V4L2_MEMORY_MMAP || buffer.index != index ||
                buffer.length < image_size || buffer.length == 0U || offset < 0 || (uint64_t)offset != buffer.m.offset)
        {
            CAM_LOGE("invalid QUERYBUF metadata: index=%u returned=%u type=%u memory=%u length=%u", index, buffer.index,
                    buffer.type, buffer.memory, buffer.length);
            return v4l2_set_error(
                    camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid QUERYBUF metadata");
        }

        void* address = v4l2_mmap_device(buffer.length, camera->fd, offset);
        if (address == MAP_FAILED)
        {
            const int error_number = errno;
            return v4l2_set_errno_error(camera, "mmap", error_number);
        }
        camera->mapped_buffers[index] = (mapped_buffer){.address = address, .length = buffer.length};
        ++camera->mapped_buffer_count;
        if (address == NULL)
        {
            CAM_LOGE("mmap returned a NULL address for index=%u", index);
            return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR,
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
        if (v4l2_ioctl_device(camera->fd, VIDIOC_QBUF, &buffer) == -1)
        {
            const int error_number = errno;
            return v4l2_set_errno_error(camera, "VIDIOC_QBUF", error_number);
        }
        camera->mapped_buffers[index].queued = true;
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @brief Owns Configure's MMAP allocation, mapping and initial queue setup. */
camstream_camera_status_t v4l2_allocate_buffers(camstream_camera_instance* camera, uint32_t image_size)
{
    struct v4l2_requestbuffers buffers = {0};
    buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffers.memory = V4L2_MEMORY_MMAP;
    buffers.count = V4L2_REQUESTED_BUFFER_COUNT;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_REQBUFS, &buffers) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t error_status =
                error_number == EINVAL ? CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        const camstream_camera_status_t status =
                v4l2_set_errno_status(camera, error_status, "VIDIOC_REQBUFS", error_number);
        if (error_number != EINVAL && error_number != EBUSY)
        {
            camera->buffer_release_required = true; /* Allocation may have partially succeeded. */
        }
        return v4l2_fail_configuration(camera, status);
    }
    if (buffers.type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffers.memory != V4L2_MEMORY_MMAP)
    {
        CAM_LOGE("invalid REQBUFS metadata: type=%u memory=%u count=%u", buffers.type, buffers.memory, buffers.count);
        camera->allocated_buffer_count = buffers.count;
        camera->buffer_release_required = buffers.count > 0U;
        const camstream_camera_status_t status = v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned an invalid buffer queue");
        return v4l2_fail_configuration(camera, status);
    }
    if (buffers.count == 0U)
    {
        CAM_LOGE("VIDIOC_REQBUFS returned zero buffers");
        const camstream_camera_status_t status =
                v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "driver allocated no MMAP buffers");
        return v4l2_fail_configuration(camera, status);
    }
    camera->allocated_buffer_count = buffers.count;
    camera->buffer_release_required = true;
    const camstream_camera_status_t status = v4l2_prepare_buffers(camera, image_size);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return v4l2_fail_configuration(camera, status);
    }
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
camstream_camera_status_t v4l2_start(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!camera->configured || camera->streaming || camera->reopen_required ||
            camera->mapped_buffer_count != camera->allocated_buffer_count)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "start requires prepared buffers");
    }
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        if (!camera->mapped_buffers[index].queued || camera->mapped_buffers[index].outstanding)
        {
            return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "start requires every buffer queued");
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    camera->streamoff_required = true;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_STREAMON, &type) == -1)
    {
        const int error_number = errno;
        const camstream_camera_status_t failure = v4l2_set_errno_error(camera, "VIDIOC_STREAMON", error_number);
        camera->reopen_required = true;
        if (v4l2_streamoff_device(camera->fd) == -1)
        {
            const int cleanup_error = errno;
            CAM_LOGW("cleanup VIDIOC_STREAMOFF: errno=%d (%s)", cleanup_error, strerror(cleanup_error));
            v4l2_append_errno_detail(camera, "cleanup", "VIDIOC_STREAMOFF", cleanup_error);
        }
        else
        {
            camera->streamoff_required = false;
        }
        return failure;
    }
    camera->streaming = true;
    v4l2_clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @brief Waits only until the caller's deadline; EINTR retries use the remaining monotonic budget. */
camstream_camera_status_t v4l2_wait_frame(camstream_camera_instance* camera, uint32_t timeout_ms)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!camera->streaming || camera->reopen_required)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "wait_frame requires active streaming");
    }
    if (timeout_ms > INT_MAX)
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "wait_frame timeout exceeds poll range");
    }

    uint64_t now;
    if (!monotonic_now_ns(&now))
    {
        return v4l2_set_errno_error(camera, "clock_gettime", errno);
    }
    if (now > UINT64_MAX - (uint64_t)timeout_ms * UINT64_C(1000000))
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "wait_frame deadline overflows");
    }
    const uint64_t deadline = now + (uint64_t)timeout_ms * UINT64_C(1000000);
    int remaining_ms = (int)timeout_ms;
    for (;;)
    {
        struct pollfd descriptor = {.fd = camera->fd, .events = POLLIN};
        const int result = v4l2_poll_device(&descriptor, remaining_ms);
        if (result > 0)
        {
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                CAM_LOGE("poll device error: revents=0x%x", (unsigned)descriptor.revents);
                return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "poll reported device error");
            }
            if ((descriptor.revents & POLLIN) != 0)
            {
                v4l2_clear_error(camera);
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
            CAM_LOGE("poll returned no capture readiness: revents=0x%x", (unsigned)descriptor.revents);
            return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "poll returned unexpected events");
        }
        if (result == 0)
        {
            return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
        }
        const int error_number = errno;
        if (error_number != EINTR)
        {
            return v4l2_set_errno_error(camera, "poll", error_number);
        }
        if (!monotonic_now_ns(&now))
        {
            return v4l2_set_errno_error(camera, "clock_gettime", errno);
        }
        if (now >= deadline)
        {
            return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
        }
        remaining_ms = (int)((deadline - now + UINT64_C(999999)) / UINT64_C(1000000));
    }
}

/** @brief Dequeues one checked single-planar image and assigns a private per-instance token. */
camstream_camera_status_t v4l2_acquire_frame(camstream_camera_instance* camera, camstream_camera_frame_v1* frame)
{
    if (camera == NULL || frame == NULL || !v4l2_valid_header(frame->abi_version, frame->struct_size, sizeof(*frame)))
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "acquire_frame received invalid output");
    }
    if (!camera->streaming || camera->reopen_required)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "acquire_frame requires active streaming");
    }
    if (camera->next_frame_token == 0U)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "private frame token space is exhausted");
    }

    struct v4l2_buffer buffer = {0};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (v4l2_ioctl_device(camera->fd, VIDIOC_DQBUF, &buffer) == -1)
    {
        const int error_number = errno;
        if (error_number == EAGAIN)
        {
            return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
        }
        return v4l2_set_errno_error(camera, "VIDIOC_DQBUF", error_number);
    }
    if (buffer.index >= camera->mapped_buffer_count)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF returned invalid index=%u", buffer.index);
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid buffer index");
    }

    mapped_buffer* mapped = &camera->mapped_buffers[buffer.index];
    if (!mapped->queued || mapped->outstanding || mapped->length == 0U)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF returned buffer without queue ownership: index=%u", buffer.index);
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned unowned buffer");
    }
    mapped->queued = false;
    if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffer.memory != V4L2_MEMORY_MMAP)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF invalid type or memory: index=%u type=%u memory=%u", buffer.index, buffer.type,
                buffer.memory);
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid frame metadata");
    }
    if ((buffer.flags & V4L2_BUF_FLAG_ERROR) != 0U)
    {
        CAM_LOGW(
                "VIDIOC_DQBUF reported a recoverable frame error: index=%u sequence=%u", buffer.index, buffer.sequence);
        struct v4l2_buffer requeue = {0};
        requeue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        requeue.memory = V4L2_MEMORY_MMAP;
        requeue.index = buffer.index;
        if (v4l2_ioctl_device(camera->fd, VIDIOC_QBUF, &requeue) == -1)
        {
            const int error_number = errno;
            camera->reopen_required = true;
            return v4l2_set_errno_error(camera, "VIDIOC_QBUF after frame error", error_number);
        }
        mapped->queued = true;
        v4l2_clear_error(camera);
        return CAMSTREAM_CAMERA_STATUS_TIMEOUT;
    }
    const uint64_t minimum_size = (uint64_t)camera->active_stride * camera->active_configuration.height;
    if (buffer.length == 0U || buffer.length > mapped->length || buffer.bytesused < minimum_size ||
            buffer.bytesused > buffer.length || buffer.field != V4L2_FIELD_NONE || mapped->address == NULL)
    {
        camera->reopen_required = true;
        CAM_LOGE("VIDIOC_DQBUF invalid metadata: index=%u length=%u bytesused=%u flags=0x%x", buffer.index,
                buffer.length, buffer.bytesused, buffer.flags);
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "driver returned invalid frame metadata");
    }

    uint64_t timestamp;
    if (!frame_timestamp_ns(&buffer, &timestamp))
    {
        const int error_number = errno;
        camera->reopen_required = true;
        return v4l2_set_errno_error(camera, "clock_gettime", error_number);
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
    v4l2_clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}

/** @brief Requeues only a matching outstanding private token; QBUF failure retains borrower ownership. */
camstream_camera_status_t v4l2_release_frame(camstream_camera_instance* camera, uint64_t frame_token)
{
    if (camera == NULL || frame_token == 0U)
    {
        return v4l2_set_error(
                camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "release_frame requires a nonzero token");
    }
    if (!camera->streaming)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "release_frame requires active streaming");
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
            if (v4l2_ioctl_device(camera->fd, VIDIOC_QBUF, &buffer) == -1)
            {
                const int error_number = errno;
                return v4l2_set_errno_error(camera, "VIDIOC_QBUF", error_number);
            }
            mapped->outstanding = false;
            mapped->frame_token = 0U;
            mapped->queued = true;
            v4l2_clear_error(camera);
            return CAMSTREAM_CAMERA_STATUS_OK;
        }
    }
    return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "frame token is unknown or stale");
}

/** @brief Stops only after all borrowed frames are returned; a failed STREAMOFF leaves streaming state for retry. */
camstream_camera_status_t v4l2_stop(camstream_camera_instance* camera)
{
    if (camera == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!camera->streaming)
    {
        return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires active streaming");
    }
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        if (camera->mapped_buffers[index].outstanding)
        {
            return v4l2_set_error(camera, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "stop requires every frame released");
        }
    }
    if (v4l2_streamoff_device(camera->fd) == -1)
    {
        const int error_number = errno;
        return v4l2_set_errno_error(camera, "VIDIOC_STREAMOFF", error_number);
    }
    camera->streaming = false;
    camera->streamoff_required = false;
    for (uint32_t index = 0U; index < camera->mapped_buffer_count; ++index)
    {
        camera->mapped_buffers[index].queued = false;
    }
    v4l2_clear_error(camera);
    return CAMSTREAM_CAMERA_STATUS_OK;
}
