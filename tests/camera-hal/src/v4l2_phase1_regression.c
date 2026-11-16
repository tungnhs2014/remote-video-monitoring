#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_hal.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "V4L2 Phase-1 regression failed at line %d: %s\n", __LINE__, #condition);                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define FAKE_FD_SLOTS 16

typedef enum fake_scenario
{
    FAKE_OK,
    FAKE_OPEN_ENODEV,
    FAKE_QUERYCAP_EIO,
    FAKE_QUERYCAP_EINTR,
    FAKE_NO_CAPTURE,
    FAKE_NO_STREAMING,
    FAKE_DEVICE_CAPS_OVERRIDE,
    FAKE_ENUM_FMT_EIO,
    FAKE_NO_MODES,
    FAKE_ENUM_SIZE_EIO,
    FAKE_ENUM_INTERVAL_EIO
} fake_scenario;

typedef enum configure_scenario
{
    CONFIG_NORMAL,
    CONFIG_ADJUSTED,
    CONFIG_G_FMT_EIO,
    CONFIG_G_PARM_EIO_PRE,
    CONFIG_G_PARM_UNSUPPORTED,
    CONFIG_S_FMT_EIO,
    CONFIG_S_FMT_MJPEG,
    CONFIG_S_FMT_BAD_LAYOUT,
    CONFIG_S_FMT_BAD_SIZE,
    CONFIG_S_FMT_ZERO_WIDTH,
    CONFIG_S_FMT_INTERLACED,
    CONFIG_S_FMT_OVERFLOW_LAYOUT,
    CONFIG_S_PARM_EIO,
    CONFIG_S_PARM_UNSUPPORTED,
    CONFIG_G_PARM_EIO_POST,
    CONFIG_G_PARM_ZERO_POST,
    CONFIG_G_PARM_ZERO_DENOMINATOR_POST,
    CONFIG_QUERYBUF_EIO,
    CONFIG_QUERYBUF_BAD_INDEX,
    CONFIG_QUERYBUF_BAD_LENGTH,
    CONFIG_MMAP_FAIL,
    CONFIG_MMAP_ZERO_ADDRESS,
    CONFIG_QBUF_FAIL,
    CONFIG_REQBUFS_FEWER,
    CONFIG_REQBUFS_MORE,
    CONFIG_REQBUFS_ZERO,
    CONFIG_REQBUFS_EINVAL,
    CONFIG_REQBUFS_ENOMEM,
    CONFIG_REQBUFS_EBUSY,
    CONFIG_REQBUFS_WRONG_TYPE
} configure_scenario;

typedef enum streaming_scenario
{
    STREAM_NORMAL,
    STREAM_ON_FAIL,
    STREAM_POLL_TIMEOUT,
    STREAM_POLL_ERROR,
    STREAM_POLL_HUP,
    STREAM_POLL_EINTR,
    STREAM_DQBUF_EAGAIN,
    STREAM_DQBUF_EIO,
    STREAM_DQBUF_BAD_INDEX,
    STREAM_DQBUF_BAD_BYTES,
    STREAM_DQBUF_BAD_TYPE,
    STREAM_DQBUF_BAD_FIELD,
    STREAM_DQBUF_DUPLICATE,
    STREAM_DQBUF_UNKNOWN_CLOCK,
    STREAM_DQBUF_FLAG_ERROR,
    STREAM_DQBUF_FLAG_ERROR_QBUF_FAIL,
    STREAM_RELEASE_QBUF_FAIL,
    STREAM_OFF_FAIL
} streaming_scenario;

static struct fake_state
{
    fake_scenario scenario;
    int open_calls;
    int close_calls;
    int ioctl_calls;
    int querycap_calls;
    int invalid_call;
    int close_fails;
    configure_scenario configure;
    streaming_scenario stream_case;
    int streamon_calls;
    int streamoff_calls;
    int poll_calls;
    int dqbuf_calls;
    uint32_t sequence;
    bool streaming_by_fd[FAKE_FD_SLOTS];
    bool queued[FAKE_FD_SLOTS][8];
    int g_fmt_calls;
    int s_fmt_calls;
    int g_parm_calls;
    int s_parm_calls;
    int reqbuf_alloc_calls;
    int reqbuf_release_calls;
    int close_with_buffers;
    int release_fails;
    int unmap_fails;
    int querybuf_calls;
    int mmap_calls;
    int munmap_calls;
    int qbuf_calls;
    int close_with_mappings;
    uint32_t mapped_by_fd[FAKE_FD_SLOTS];
    uint32_t queued_by_fd[FAKE_FD_SLOTS];
    bool mapped[FAKE_FD_SLOTS][8];
    uint32_t allocated_by_fd[FAKE_FD_SLOTS];
    uint32_t last_requested_buffer_count;
    uint32_t last_returned_buffer_count;
    struct v4l2_format current_format;
    struct v4l2_streamparm current_interval;
    struct v4l2_fract requested_interval;
} fake;

static unsigned char fake_map_addresses[FAKE_FD_SLOTS][8];

static void reset_fake(fake_scenario scenario)
{
    memset(&fake, 0, sizeof(fake));
    fake.scenario = scenario;
    fake.current_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fake.current_format.fmt.pix.width = 640U;
    fake.current_format.fmt.pix.height = 480U;
    fake.current_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fake.current_format.fmt.pix.field = V4L2_FIELD_NONE;
    fake.current_format.fmt.pix.bytesperline = 1280U;
    fake.current_format.fmt.pix.sizeimage = 614400U;
    fake.current_interval.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fake.current_interval.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    fake.current_interval.parm.capture.timeperframe.numerator = 1U;
    fake.current_interval.parm.capture.timeperframe.denominator = 30U;
}

static int fail_with(int error_number)
{
    errno = error_number;
    return -1;
}

int camstream_test_v4l2_open(const char* path, int flags)
{
    ++fake.open_calls;
    if (strcmp(path, "/dev/video-test") != 0 || (flags & O_ACCMODE) != O_RDWR || (flags & O_NONBLOCK) == 0 ||
            (flags & O_CLOEXEC) == 0)
    {
        fake.invalid_call = 1;
        return fail_with(EINVAL);
    }
    if (fake.scenario == FAKE_OPEN_ENODEV)
    {
        return fail_with(ENODEV);
    }
    return 100 + fake.open_calls;
}

int camstream_test_v4l2_ioctl(int fd, unsigned long request, void* argument)
{
    ++fake.ioctl_calls;
    if (fd < 101 || fd >= 100 + FAKE_FD_SLOTS || argument == NULL)
    {
        fake.invalid_call = 1;
        return fail_with(EINVAL);
    }
    if (request == VIDIOC_QUERYCAP)
    {
        ++fake.querycap_calls;
        if (fake.scenario == FAKE_QUERYCAP_EIO)
        {
            return fail_with(EIO);
        }
        if (fake.scenario == FAKE_QUERYCAP_EINTR && fake.querycap_calls == 1)
        {
            return fail_with(EINTR);
        }
        struct v4l2_capability* capability = argument;
        memset(capability, 0, sizeof(*capability));
        capability->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        if (fake.scenario == FAKE_NO_CAPTURE)
        {
            capability->capabilities = V4L2_CAP_STREAMING;
        }
        if (fake.scenario == FAKE_NO_STREAMING)
        {
            capability->capabilities = V4L2_CAP_VIDEO_CAPTURE;
        }
        if (fake.scenario == FAKE_DEVICE_CAPS_OVERRIDE)
        {
            capability->capabilities |= V4L2_CAP_DEVICE_CAPS;
            capability->device_caps = V4L2_CAP_STREAMING;
        }
        return 0;
    }
    if (request == VIDIOC_ENUM_FMT)
    {
        if (fake.scenario == FAKE_ENUM_FMT_EIO)
        {
            return fail_with(EIO);
        }
        struct v4l2_fmtdesc* format = argument;
        if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.scenario == FAKE_NO_MODES || format->index >= 2U)
        {
            return fail_with(EINVAL);
        }
        format->pixelformat = format->index == 0U ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
        return 0;
    }
    if (request == VIDIOC_ENUM_FRAMESIZES)
    {
        if (fake.scenario == FAKE_ENUM_SIZE_EIO)
        {
            return fail_with(EIO);
        }
        struct v4l2_frmsizeenum* size = argument;
        if (size->pixel_format != V4L2_PIX_FMT_YUYV)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (size->index >= 2U)
        {
            return fail_with(EINVAL);
        }
        if (size->index == 0U)
        {
            size->type = V4L2_FRMSIZE_TYPE_STEPWISE;
        }
        else
        {
            size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
            size->discrete.width = 640U;
            size->discrete.height = 480U;
        }
        return 0;
    }
    if (request == VIDIOC_ENUM_FRAMEINTERVALS)
    {
        if (fake.scenario == FAKE_ENUM_INTERVAL_EIO)
        {
            return fail_with(EIO);
        }
        struct v4l2_frmivalenum* interval = argument;
        if (interval->pixel_format != V4L2_PIX_FMT_YUYV || interval->width != 640U || interval->height != 480U)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (interval->index >= 4U)
        {
            return fail_with(EINVAL);
        }
        interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        interval->discrete.numerator = interval->index == 0U ? 0U : 1U;
        interval->discrete.denominator = interval->index == 3U ? 15U : 30U;
        return 0;
    }
    if (request == VIDIOC_G_FMT)
    {
        struct v4l2_format* format = argument;
        ++fake.g_fmt_calls;
        if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.configure == CONFIG_G_FMT_EIO)
        {
            return fail_with(EIO);
        }
        *format = fake.current_format;
        return 0;
    }
    if (request == VIDIOC_S_FMT)
    {
        struct v4l2_format* format = argument;
        ++fake.s_fmt_calls;
        if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_EIO)
        {
            return fail_with(EIO);
        }
        if (fake.allocated_by_fd[fd - 100] != 0U)
        {
            return fail_with(EBUSY);
        }
        fake.current_format = *format;
        fake.current_format.fmt.pix.field = V4L2_FIELD_NONE;
        if (fake.s_fmt_calls == 1 && (fake.configure == CONFIG_ADJUSTED || fake.configure == CONFIG_G_PARM_EIO_POST))
        {
            fake.current_format.fmt.pix.width = 320U;
            fake.current_format.fmt.pix.height = 240U;
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_MJPEG)
        {
            fake.current_format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_OVERFLOW_LAYOUT)
        {
            fake.current_format.fmt.pix.width = UINT32_MAX - 1U;
            fake.current_format.fmt.pix.height = 2U;
            fake.current_format.fmt.pix.bytesperline = UINT32_MAX;
            fake.current_format.fmt.pix.sizeimage = UINT32_MAX;
        }
        else
        {
            fake.current_format.fmt.pix.bytesperline = fake.current_format.fmt.pix.width * 2U;
            fake.current_format.fmt.pix.sizeimage =
                    fake.current_format.fmt.pix.bytesperline * fake.current_format.fmt.pix.height;
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_BAD_LAYOUT)
        {
            fake.current_format.fmt.pix.bytesperline = 1U;
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_BAD_SIZE)
        {
            fake.current_format.fmt.pix.sizeimage = 1U;
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_ZERO_WIDTH)
        {
            fake.current_format.fmt.pix.width = 0U;
        }
        if (fake.s_fmt_calls == 1 && fake.configure == CONFIG_S_FMT_INTERLACED)
        {
            fake.current_format.fmt.pix.field = V4L2_FIELD_INTERLACED;
        }
        *format = fake.current_format;
        return 0;
    }
    if (request == VIDIOC_G_PARM)
    {
        struct v4l2_streamparm* interval = argument;
        ++fake.g_parm_calls;
        if (interval->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if ((fake.g_parm_calls == 1 && fake.configure == CONFIG_G_PARM_EIO_PRE) ||
                (fake.g_parm_calls == 2 && fake.configure == CONFIG_G_PARM_EIO_POST))
        {
            return fail_with(EIO);
        }
        *interval = fake.current_interval;
        if (fake.configure == CONFIG_G_PARM_UNSUPPORTED)
        {
            interval->parm.capture.capability = 0U;
        }
        if (fake.g_parm_calls == 2 && fake.configure == CONFIG_G_PARM_ZERO_POST)
        {
            interval->parm.capture.timeperframe.numerator = 0U;
        }
        if (fake.g_parm_calls == 2 && fake.configure == CONFIG_G_PARM_ZERO_DENOMINATOR_POST)
        {
            interval->parm.capture.timeperframe.denominator = 0U;
        }
        return 0;
    }
    if (request == VIDIOC_S_PARM)
    {
        struct v4l2_streamparm* interval = argument;
        ++fake.s_parm_calls;
        if (interval->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.s_parm_calls == 1)
        {
            fake.requested_interval = interval->parm.capture.timeperframe;
            if (fake.configure == CONFIG_S_PARM_UNSUPPORTED)
            {
                return fail_with(ENOTTY);
            }
            if (fake.configure == CONFIG_S_PARM_EIO)
            {
                return fail_with(EIO);
            }
        }
        fake.current_interval = *interval;
        fake.current_interval.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        if (fake.s_parm_calls == 1 && fake.configure == CONFIG_ADJUSTED)
        {
            fake.current_interval.parm.capture.timeperframe.numerator = 1001U;
            fake.current_interval.parm.capture.timeperframe.denominator = 15000U;
        }
        *interval = fake.current_interval;
        return 0;
    }
    if (request == VIDIOC_REQBUFS)
    {
        struct v4l2_requestbuffers* buffers = argument;
        if (buffers->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffers->memory != V4L2_MEMORY_MMAP ||
                buffers->capabilities != 0U || buffers->reserved[0] != 0U)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        const int slot = fd - 100;
        if (buffers->count == 0U)
        {
            ++fake.reqbuf_release_calls;
            if (fake.mapped_by_fd[slot] != 0U)
            {
                fake.invalid_call = 1;
                return fail_with(EBUSY);
            }
            if (fake.release_fails != 0)
            {
                return fail_with(EIO);
            }
            fake.allocated_by_fd[slot] = 0U;
            fake.queued_by_fd[slot] = 0U;
            memset(fake.queued[slot], 0, sizeof(fake.queued[slot]));
            return 0;
        }
        ++fake.reqbuf_alloc_calls;
        if (buffers->count != 4U || fake.allocated_by_fd[slot] != 0U)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        fake.last_requested_buffer_count = buffers->count;
        if (fake.configure == CONFIG_REQBUFS_EINVAL)
        {
            return fail_with(EINVAL);
        }
        if (fake.configure == CONFIG_REQBUFS_ENOMEM)
        {
            fake.allocated_by_fd[slot] = 2U;
            return fail_with(ENOMEM);
        }
        if (fake.configure == CONFIG_REQBUFS_EBUSY)
        {
            return fail_with(EBUSY);
        }
        if (fake.configure == CONFIG_REQBUFS_FEWER)
        {
            buffers->count = 2U;
        }
        else if (fake.configure == CONFIG_REQBUFS_MORE)
        {
            buffers->count = 6U;
        }
        else if (fake.configure == CONFIG_REQBUFS_ZERO)
        {
            buffers->count = 0U;
        }
        if (fake.configure == CONFIG_REQBUFS_WRONG_TYPE)
        {
            buffers->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        }
        fake.last_returned_buffer_count = buffers->count;
        fake.allocated_by_fd[slot] = buffers->count;
        return 0;
    }
    if (request == VIDIOC_QUERYBUF)
    {
        struct v4l2_buffer* buffer = argument;
        const int slot = fd - 100;
        ++fake.querybuf_calls;
        if (buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffer->memory != V4L2_MEMORY_MMAP ||
                buffer->index >= fake.allocated_by_fd[slot])
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.configure == CONFIG_QUERYBUF_EIO && buffer->index == 1U)
        {
            return fail_with(EIO);
        }
        const uint32_t index = buffer->index;
        buffer->length = fake.configure == CONFIG_QUERYBUF_BAD_LENGTH && index == 1U ? 0U : 614400U;
        buffer->m.offset = index * 4096U;
        if (fake.configure == CONFIG_QUERYBUF_BAD_INDEX && index == 1U)
        {
            buffer->index = 7U;
        }
        return 0;
    }
    if (request == VIDIOC_QBUF)
    {
        struct v4l2_buffer* buffer = argument;
        const int slot = fd - 100;
        ++fake.qbuf_calls;
        if (buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buffer->memory != V4L2_MEMORY_MMAP ||
                buffer->index >= fake.allocated_by_fd[slot] || !fake.mapped[slot][buffer->index] ||
                fake.mapped_by_fd[slot] != fake.allocated_by_fd[slot] || fake.queued[slot][buffer->index])
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if ((fake.configure == CONFIG_QBUF_FAIL && buffer->index == 1U) ||
                ((fake.stream_case == STREAM_RELEASE_QBUF_FAIL ||
                         fake.stream_case == STREAM_DQBUF_FLAG_ERROR_QBUF_FAIL) &&
                        fake.streaming_by_fd[slot]))
        {
            return fail_with(EIO);
        }
        ++fake.queued_by_fd[slot];
        fake.queued[slot][buffer->index] = true;
        return 0;
    }
    if (request == VIDIOC_STREAMON)
    {
        enum v4l2_buf_type* type = argument;
        const int slot = fd - 100;
        ++fake.streamon_calls;
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE || fake.streaming_by_fd[slot] ||
                fake.queued_by_fd[slot] != fake.allocated_by_fd[slot])
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.stream_case == STREAM_ON_FAIL)
        {
            return fail_with(EIO);
        }
        fake.streaming_by_fd[slot] = true;
        return 0;
    }
    if (request == VIDIOC_STREAMOFF)
    {
        enum v4l2_buf_type* type = argument;
        const int slot = fd - 100;
        ++fake.streamoff_calls;
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.stream_case == STREAM_OFF_FAIL)
        {
            return fail_with(EIO);
        }
        fake.streaming_by_fd[slot] = false;
        fake.queued_by_fd[slot] = 0U;
        memset(fake.queued[slot], 0, sizeof(fake.queued[slot]));
        return 0;
    }
    if (request == VIDIOC_DQBUF)
    {
        struct v4l2_buffer* buffer = argument;
        const int slot = fd - 100;
        ++fake.dqbuf_calls;
        if (!fake.streaming_by_fd[slot] || buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
                buffer->memory != V4L2_MEMORY_MMAP)
        {
            fake.invalid_call = 1;
            return fail_with(EINVAL);
        }
        if (fake.stream_case == STREAM_DQBUF_EAGAIN)
        {
            return fail_with(EAGAIN);
        }
        if (fake.stream_case == STREAM_DQBUF_EIO)
        {
            return fail_with(EIO);
        }
        if (fake.stream_case == STREAM_DQBUF_DUPLICATE)
        {
            buffer->index = 0U;
            buffer->length = 614400U;
            buffer->bytesused = 614400U;
            buffer->field = V4L2_FIELD_NONE;
            buffer->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            return 0;
        }
        uint32_t index = 0U;
        while (index < fake.allocated_by_fd[slot] && !fake.queued[slot][index])
        {
            ++index;
        }
        if (index == fake.allocated_by_fd[slot])
        {
            return fail_with(EAGAIN);
        }
        fake.queued[slot][index] = false;
        --fake.queued_by_fd[slot];
        buffer->index = index;
        buffer->length = 614400U;
        buffer->bytesused = 614400U;
        buffer->field = V4L2_FIELD_NONE;
        buffer->sequence = ++fake.sequence;
        buffer->flags = fake.stream_case == STREAM_DQBUF_UNKNOWN_CLOCK ? V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN
                                                                       : V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        if (fake.stream_case == STREAM_DQBUF_FLAG_ERROR || fake.stream_case == STREAM_DQBUF_FLAG_ERROR_QBUF_FAIL)
        {
            buffer->flags |= V4L2_BUF_FLAG_ERROR;
            buffer->bytesused = 0U;
            buffer->field = V4L2_FIELD_INTERLACED;
        }
        buffer->timestamp.tv_sec = 10;
        buffer->timestamp.tv_usec = 123000;
        if (fake.stream_case == STREAM_DQBUF_BAD_INDEX)
        {
            buffer->index = 9U;
        }
        if (fake.stream_case == STREAM_DQBUF_BAD_BYTES)
        {
            buffer->bytesused = 614401U;
        }
        if (fake.stream_case == STREAM_DQBUF_BAD_TYPE)
        {
            buffer->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        }
        if (fake.stream_case == STREAM_DQBUF_BAD_FIELD)
        {
            buffer->field = V4L2_FIELD_INTERLACED;
        }
        return 0;
    }
    fake.invalid_call = 1;
    return fail_with(EINVAL);
}

int camstream_test_v4l2_close(int fd)
{
    ++fake.close_calls;
    if (fd < 101 || fd >= 100 + FAKE_FD_SLOTS)
    {
        fake.invalid_call = 1;
        return fail_with(EBADF);
    }
    const bool report_failure = fake.close_fails != 0;
    if (fake.mapped_by_fd[fd - 100] != 0U)
    {
        ++fake.close_with_mappings;
    }
    if (fake.allocated_by_fd[fd - 100] != 0U)
    {
        ++fake.close_with_buffers;
        fake.allocated_by_fd[fd - 100] = 0U;
    }
    fake.streaming_by_fd[fd - 100] = false;
    fake.queued_by_fd[fd - 100] = 0U;
    if (report_failure)
    {
        return fail_with(EIO);
    }
    return 0;
}

int camstream_test_v4l2_poll(struct pollfd* descriptor, int timeout_ms)
{
    ++fake.poll_calls;
    if (descriptor == NULL || descriptor->fd < 101 || descriptor->fd >= 100 + FAKE_FD_SLOTS ||
            descriptor->events != POLLIN || timeout_ms < 0)
    {
        fake.invalid_call = 1;
        return fail_with(EINVAL);
    }
    const int slot = descriptor->fd - 100;
    if (!fake.streaming_by_fd[slot])
    {
        fake.invalid_call = 1;
        return fail_with(EINVAL);
    }
    if (fake.stream_case == STREAM_POLL_EINTR && fake.poll_calls == 1)
    {
        return fail_with(EINTR);
    }
    if (fake.stream_case == STREAM_POLL_ERROR)
    {
        return fail_with(EIO);
    }
    if (fake.stream_case == STREAM_POLL_HUP)
    {
        descriptor->revents = POLLHUP;
        return 1;
    }
    if (fake.stream_case == STREAM_POLL_TIMEOUT || fake.queued_by_fd[slot] == 0U)
    {
        return 0;
    }
    descriptor->revents = POLLIN;
    return 1;
}

void* camstream_test_v4l2_mmap(size_t length, int protection, int flags, int fd, off_t offset)
{
    ++fake.mmap_calls;
    if (protection != (PROT_READ | PROT_WRITE) || flags != MAP_SHARED || fd < 101 || fd >= 100 + FAKE_FD_SLOTS ||
            length != 614400U || offset < 0 || (uint64_t)offset % 4096U != 0U)
    {
        fake.invalid_call = 1;
        errno = EINVAL;
        return MAP_FAILED;
    }
    const int slot = fd - 100;
    const uint32_t index = (uint32_t)offset / 4096U;
    if (index >= fake.allocated_by_fd[slot] || fake.mapped[slot][index])
    {
        fake.invalid_call = 1;
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (fake.configure == CONFIG_MMAP_FAIL && index == 1U)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    fake.mapped[slot][index] = true;
    ++fake.mapped_by_fd[slot];
    if (fake.configure == CONFIG_MMAP_ZERO_ADDRESS && index == 0U)
    {
        return NULL;
    }
    return &fake_map_addresses[slot][index];
}

int camstream_test_v4l2_munmap(void* address, size_t length)
{
    ++fake.munmap_calls;
    if (length != 614400U)
    {
        fake.invalid_call = 1;
        return fail_with(EINVAL);
    }
    if (address == NULL && fake.mapped[1][0])
    {
        fake.mapped[1][0] = false;
        --fake.mapped_by_fd[1];
        return 0;
    }
    for (int slot = 1; slot < FAKE_FD_SLOTS; ++slot)
    {
        for (uint32_t index = 0U; index < 8U; ++index)
        {
            if (address == &fake_map_addresses[slot][index] && fake.mapped[slot][index])
            {
                if (fake.unmap_fails != 0)
                {
                    return fail_with(EIO);
                }
                fake.mapped[slot][index] = false;
                --fake.mapped_by_fd[slot];
                return 0;
            }
        }
    }
    fake.invalid_call = 1;
    return fail_with(EINVAL);
}

static int test_success(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.open_calls == 1 && fake.invalid_call == 0);

    camstream_camera_capabilities_v1 capabilities = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(capabilities),
    };
    CHECK(camstream_camera_get_capabilities(camera, &capabilities) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(capabilities.stream_config_count == 2U && capabilities.maximum_plane_count == 1U);

    for (uint32_t index = 0U; index < 2U; ++index)
    {
        camstream_camera_stream_config_v1 configuration = {
                .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                .struct_size = sizeof(configuration),
        };
        CHECK(camstream_camera_get_stream_configuration(camera, index, &configuration) == CAMSTREAM_CAMERA_STATUS_OK);
        CHECK(strcmp(configuration.source_identifier, "/dev/video-test") == 0);
        CHECK(configuration.width == 640U && configuration.height == 480U);
        CHECK(configuration.pixel_format == CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV);
        CHECK(configuration.frame_rate_numerator == (index == 0U ? 30U : 15U));
        CHECK(configuration.frame_rate_denominator == 1U);
        camstream_camera_stream_config_v1 active = {
                .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                .struct_size = sizeof(active),
        };
        configuration.pixel_format = CAMSTREAM_CAMERA_FOURCC('M', 'J', 'P', 'G');
        CHECK(camstream_camera_configure(camera, &configuration, &active) == CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED);
    }
    camstream_camera_stream_config_v1 missing = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(missing),
    };
    CHECK(camstream_camera_get_stream_configuration(camera, 2U, &missing) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.close_calls == 1 && fake.invalid_call == 0);
    return 0;
}

static int test_open_failure(
        fake_scenario scenario, camstream_camera_status_t expected, int expected_closes, const char* diagnostic)
{
    reset_fake(scenario);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == expected);
    char error[256] = {0};
    CHECK(camstream_camera_get_last_error(camera, error, sizeof(error)) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(strstr(error, diagnostic) != NULL);
    camstream_camera_destroy(camera);
    CHECK(fake.close_calls == expected_closes && fake.invalid_call == 0);
    return 0;
}

static int test_eintr_and_close_failure(void)
{
    reset_fake(FAKE_QUERYCAP_EINTR);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(fake.querycap_calls == 1 && fake.close_calls == 1);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    fake.close_fails = 1;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    fake.close_fails = 0;
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.close_calls == 3 && fake.invalid_call == 0);
    return 0;
}

static int test_instance_isolation(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* first = NULL;
    camstream_camera* second = NULL;
    CHECK(camstream_camera_create(&first) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_create(&second) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(first, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(second, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(first) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_capabilities_v1 capabilities = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(capabilities),
    };
    CHECK(camstream_camera_get_capabilities(second, &capabilities) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(capabilities.stream_config_count == 2U);
    CHECK(camstream_camera_close(second) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(first);
    camstream_camera_destroy(second);
    CHECK(fake.open_calls == 2 && fake.close_calls == 2 && fake.invalid_call == 0);
    return 0;
}

static int test_invalid_source(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, NULL) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_open(camera, "") == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    char overlong[CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE + 1U];
    memset(overlong, 'x', sizeof(overlong));
    overlong[sizeof(overlong) - 1U] = '\0';
    CHECK(camstream_camera_open(camera, overlong) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    camstream_camera_destroy(camera);
    CHECK(fake.open_calls == 0 && fake.close_calls == 0 && fake.ioctl_calls == 0 && fake.invalid_call == 0);
    return 0;
}

static camstream_camera_stream_config_v1 empty_configuration(void)
{
    const camstream_camera_stream_config_v1 configuration = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(configuration),
    };
    return configuration;
}

static int open_mode(camstream_camera** camera, camstream_camera_stream_config_v1* requested)
{
    CHECK(camstream_camera_create(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(*camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    *requested = empty_configuration();
    CHECK(camstream_camera_get_stream_configuration(*camera, 0U, requested) == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

static int test_configuration_success(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(fake.g_fmt_calls == 0 && fake.s_fmt_calls == 0 && fake.g_parm_calls == 0 && fake.s_parm_calls == 0);
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(active.width == 640U && active.height == 480U);
    CHECK(active.pixel_format == CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV);
    CHECK(active.frame_rate_numerator == 30U && active.frame_rate_denominator == 1U);
    CHECK(strcmp(active.source_identifier, "/dev/video-test") == 0);
    CHECK(fake.g_fmt_calls == 1 && fake.s_fmt_calls == 1 && fake.g_parm_calls == 2 && fake.s_parm_calls == 1);
    CHECK(fake.requested_interval.numerator == 1U && fake.requested_interval.denominator == 30U);
    CHECK(fake.reqbuf_alloc_calls == 1 && fake.last_requested_buffer_count == 4U);
    CHECK(fake.last_returned_buffer_count == 4U && fake.allocated_by_fd[1] == 4U);
    CHECK(fake.querybuf_calls == 4 && fake.mmap_calls == 4 && fake.qbuf_calls == 4);
    CHECK(fake.mapped_by_fd[1] == 4U && fake.queued_by_fd[1] == 4U);
    const int configured_ioctls = fake.ioctl_calls;
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(fake.ioctl_calls == configured_ioctls);
    CHECK(camstream_camera_get_stream_configuration(camera, 0U, &requested) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_wait_frame(camera, 0U) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.reqbuf_release_calls == 1 && fake.allocated_by_fd[1] == 0U);
    CHECK(fake.munmap_calls == 4 && fake.mapped_by_fd[1] == 0U && fake.close_with_mappings == 0);
    CHECK(fake.close_calls == 1 && fake.close_with_buffers == 0 && fake.invalid_call == 0);
    return 0;
}

static int test_buffer_count(configure_scenario scenario, uint32_t granted_count)
{
    reset_fake(FAKE_OK);
    fake.configure = scenario;
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(active.width == requested.width && active.frame_rate_numerator == requested.frame_rate_numerator);
    CHECK(fake.reqbuf_alloc_calls == 1 && fake.last_requested_buffer_count == 4U);
    CHECK(fake.last_returned_buffer_count == granted_count && fake.allocated_by_fd[1] == granted_count);
    CHECK(fake.querybuf_calls == (int)granted_count && fake.mmap_calls == (int)granted_count);
    CHECK(fake.qbuf_calls == (int)granted_count && fake.mapped_by_fd[1] == granted_count);
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.reqbuf_release_calls == 1 && fake.allocated_by_fd[1] == 0U);
    CHECK(fake.munmap_calls == (int)granted_count && fake.mapped_by_fd[1] == 0U);
    CHECK(fake.close_with_buffers == 0 && fake.invalid_call == 0);
    return 0;
}

static int test_zero_address_mapping(void)
{
    reset_fake(FAKE_OK);
    fake.configure = CONFIG_MMAP_ZERO_ADDRESS;
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR);
    CHECK(active.width == 0U && active.height == 0U);
    CHECK(fake.mmap_calls == 1 && fake.munmap_calls == 1 && fake.qbuf_calls == 0);
    CHECK(fake.mapped_by_fd[1] == 0U && fake.allocated_by_fd[1] == 0U && fake.reqbuf_release_calls == 1);
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    fake.configure = CONFIG_NORMAL;
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.mapped_by_fd[1] == 0U && fake.munmap_calls == 5 && fake.invalid_call == 0);
    camstream_camera_destroy(camera);
    return 0;
}

static int test_driver_adjustment(void)
{
    reset_fake(FAKE_OK);
    fake.configure = CONFIG_ADJUSTED;
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(requested.width == 640U && requested.height == 480U);
    CHECK(active.width == 320U && active.height == 240U);
    CHECK(active.frame_rate_numerator == 15000U && active.frame_rate_denominator == 1001U);
    CHECK(fake.current_format.fmt.pix.bytesperline == 640U && fake.current_format.fmt.pix.sizeimage == 153600U);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0);
    return 0;
}

static int test_request_validation(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 valid;
    CHECK(open_mode(&camera, &valid) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    camstream_camera_stream_config_v1 request = valid;

    request.pixel_format = CAMSTREAM_CAMERA_FOURCC('M', 'J', 'P', 'G');
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED);
    request = valid;
    request.width = 0U;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    request.height = 0U;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    request.width = UINT32_MAX;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    request.width = 800U;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED);
    request = valid;
    request.frame_rate_numerator = 0U;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    request.frame_rate_denominator = 0U;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    request.abi_version++;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    request.struct_size--;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    memcpy(request.source_identifier, "/dev/other", sizeof("/dev/other"));
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    request = valid;
    active.abi_version++;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    active = empty_configuration();
    active.struct_size--;
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(fake.g_fmt_calls == 0 && fake.s_fmt_calls == 0 && fake.g_parm_calls == 0 && fake.s_parm_calls == 0);
    active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &valid, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(active.width == valid.width && active.frame_rate_numerator == valid.frame_rate_numerator);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);

    reset_fake(FAKE_OK);
    camera = NULL;
    request = valid;
    active = empty_configuration();
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_configure(camera, &request, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    camstream_camera_destroy(camera);
    CHECK(fake.open_calls == 0 && fake.ioctl_calls == 0);
    return 0;
}

static int test_oversized_structures(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 mode;
    CHECK(open_mode(&camera, &mode) == 0);
    struct extended_configuration
    {
        camstream_camera_stream_config_v1 base;
        uint8_t extension[8];
    } requested = {0}, active = {0};
    requested.base = mode;
    requested.base.struct_size = sizeof(requested);
    memset(active.extension, 0xA5, sizeof(active.extension));
    active.base = empty_configuration();
    active.base.struct_size = sizeof(active);
    CHECK(camstream_camera_configure(camera, &requested.base, &active.base) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(active.base.width == 640U && active.base.frame_rate_numerator == 30U);
    for (size_t index = 0U; index < sizeof(active.extension); ++index)
    {
        CHECK(active.extension[index] == 0xA5U);
    }
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    return 0;
}

static int test_configuration_failure(
        configure_scenario scenario, camstream_camera_status_t expected, int expected_s_fmt, const char* diagnostic)
{
    reset_fake(FAKE_OK);
    fake.configure = scenario;
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active;
    memset(&active, 0xA5, sizeof(active));
    active.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    active.struct_size = sizeof(active);
    uint8_t before[sizeof(active)];
    memcpy(before, &active, sizeof(before));
    CHECK(camstream_camera_configure(camera, &requested, &active) == expected);
    CHECK(memcmp(&active, before, sizeof(active)) == 0);
    CHECK(fake.s_fmt_calls == (expected_s_fmt == 2 ? 1 : expected_s_fmt));
    char error[256] = {0};
    CHECK(camstream_camera_get_last_error(camera, error, sizeof(error)) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(strstr(error, diagnostic) != NULL);
    if (scenario >= CONFIG_REQBUFS_FEWER)
    {
        CHECK(fake.reqbuf_alloc_calls == 1 && fake.last_requested_buffer_count == 4U);
        const int cleanup_calls = scenario == CONFIG_REQBUFS_ENOMEM || scenario == CONFIG_REQBUFS_WRONG_TYPE ? 1 : 0;
        CHECK(fake.reqbuf_release_calls == cleanup_calls);
        CHECK(fake.allocated_by_fd[1] == 0U);
    }
    camstream_camera_stream_config_v1 visible = empty_configuration();
    CHECK(camstream_camera_get_stream_configuration(camera, 0U, &visible) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(visible.width == requested.width && visible.frame_rate_numerator == requested.frame_rate_numerator);
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    if (scenario >= CONFIG_S_FMT_EIO)
    {
        const int expected_s_parm = (scenario == CONFIG_S_FMT_MJPEG || scenario == CONFIG_S_FMT_BAD_LAYOUT ||
                                            scenario == CONFIG_S_FMT_BAD_SIZE || scenario == CONFIG_S_FMT_ZERO_WIDTH ||
                                            scenario == CONFIG_S_FMT_INTERLACED ||
                                            scenario == CONFIG_S_FMT_OVERFLOW_LAYOUT || scenario == CONFIG_S_FMT_EIO)
                                            ? 0
                                            : 1;
        CHECK(fake.s_parm_calls == expected_s_parm);
        const int failed_ioctls = fake.ioctl_calls;
        CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
        CHECK(fake.ioctl_calls == failed_ioctls);
        CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
        CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    }
    fake.configure = CONFIG_NORMAL;
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0);
    return 0;
}

static int test_repeated_cycles_and_instances(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* first = NULL;
    CHECK(camstream_camera_create(&first) == CAMSTREAM_CAMERA_STATUS_OK);
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        camstream_camera_stream_config_v1 requested = empty_configuration();
        CHECK(camstream_camera_open(first, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
        CHECK(camstream_camera_get_stream_configuration(first, (uint32_t)cycle, &requested) ==
                CAMSTREAM_CAMERA_STATUS_OK);
        camstream_camera_stream_config_v1 active = empty_configuration();
        CHECK(camstream_camera_configure(first, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
        CHECK(active.frame_rate_numerator == (cycle == 0 ? 30U : 15U));
        CHECK(active.frame_rate_denominator == 1U);
        CHECK(camstream_camera_close(first) == CAMSTREAM_CAMERA_STATUS_OK);
    }
    camstream_camera_destroy(first);
    CHECK(fake.open_calls == 2 && fake.close_calls == 2 && fake.reqbuf_release_calls == 2);

    reset_fake(FAKE_OK);
    camstream_camera* second = NULL;
    CHECK(camstream_camera_create(&first) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_create(&second) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(first, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(second, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_stream_config_v1 requested = empty_configuration();
    CHECK(camstream_camera_get_stream_configuration(first, 0U, &requested) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(first, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(active.width == 640U && active.frame_rate_numerator == 30U);
    CHECK(camstream_camera_get_stream_configuration(second, 0U, &requested) == CAMSTREAM_CAMERA_STATUS_OK);
    active = empty_configuration();
    CHECK(camstream_camera_configure(second, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(active.width == 640U && active.frame_rate_numerator == 30U);
    CHECK(fake.allocated_by_fd[1] == 4U && fake.allocated_by_fd[2] == 4U);
    CHECK(camstream_camera_close(first) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.allocated_by_fd[1] == 0U && fake.allocated_by_fd[2] == 4U);
    CHECK(camstream_camera_close(second) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(first);
    camstream_camera_destroy(second);
    CHECK(fake.open_calls == 2 && fake.close_calls == 2 && fake.invalid_call == 0);
    return 0;
}

static int test_buffer_release_failure(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);

    fake.release_fails = 1;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    char error[256] = {0};
    CHECK(camstream_camera_get_last_error(camera, error, sizeof(error)) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(strstr(error, "VIDIOC_REQBUFS(count=0) failed") != NULL);
    CHECK(fake.reqbuf_release_calls == 1 && fake.close_with_buffers == 1);
    fake.release_fails = 0;
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.close_calls == 2 && fake.invalid_call == 0);

    reset_fake(FAKE_OK);
    CHECK(open_mode(&camera, &requested) == 0);
    active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.reqbuf_release_calls == 1 && fake.close_calls == 1 && fake.close_with_buffers == 0);
    return 0;
}

static int test_unmap_failure_retains_ownership(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    fake.unmap_fails = 1;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(fake.close_calls == 0 && fake.reqbuf_release_calls == 0 && fake.mapped_by_fd[1] == 4U);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    fake.unmap_fails = 0;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.mapped_by_fd[1] == 0U && fake.close_calls == 1 && fake.close_with_mappings == 0);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0);
    return 0;
}

static int test_buffer_preparation_failure(configure_scenario scenario, camstream_camera_status_t expected,
        const char* diagnostic, int expected_mappings, int expected_queues)
{
    reset_fake(FAKE_OK);
    fake.configure = scenario;
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    active.width = 123U;
    CHECK(camstream_camera_configure(camera, &requested, &active) == expected);
    CHECK(active.width == 123U);
    CHECK(fake.mmap_calls == expected_mappings && fake.qbuf_calls == expected_queues);
    CHECK(fake.mapped_by_fd[1] == 0U && fake.allocated_by_fd[1] == 0U);
    CHECK(fake.munmap_calls == (scenario == CONFIG_MMAP_FAIL ? 1
                                       : scenario == CONFIG_QUERYBUF_EIO || scenario == CONFIG_QUERYBUF_BAD_INDEX ||
                                                       scenario == CONFIG_QUERYBUF_BAD_LENGTH
                                               ? 1
                                               : 4));
    CHECK(fake.close_with_mappings == 0 && fake.invalid_call == 0);
    char error[256] = {0};
    CHECK(camstream_camera_get_last_error(camera, error, sizeof(error)) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(strstr(error, diagnostic) != NULL);
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    fake.configure = CONFIG_NORMAL;
    active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0 && fake.close_with_mappings == 0);
    return 0;
}

static int test_request_cleanup_failure(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    fake.configure = CONFIG_REQBUFS_ENOMEM;
    fake.release_fails = 1;
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(active.width == 0U && fake.allocated_by_fd[1] == 2U);
    char error[256] = {0};
    CHECK(camstream_camera_get_last_error(camera, error, sizeof(error)) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(strstr(error, "VIDIOC_REQBUFS failed") != NULL);
    CHECK(strstr(error, "cleanup VIDIOC_REQBUFS(count=0) failed") != NULL);
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);

    fake.release_fails = 0;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.allocated_by_fd[1] == 0U && fake.invalid_call == 0);
    return 0;
}

static int test_instance_fault_isolation(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* faulted = NULL;
    camstream_camera* healthy = NULL;
    camstream_camera_stream_config_v1 faulted_request;
    camstream_camera_stream_config_v1 healthy_request;
    CHECK(open_mode(&faulted, &faulted_request) == 0);
    CHECK(open_mode(&healthy, &healthy_request) == 0);

    fake.configure = CONFIG_S_PARM_EIO;
    camstream_camera_stream_config_v1 faulted_active = empty_configuration();
    CHECK(camstream_camera_configure(faulted, &faulted_request, &faulted_active) ==
            CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    fake.configure = CONFIG_NORMAL;
    camstream_camera_stream_config_v1 healthy_active = empty_configuration();
    CHECK(camstream_camera_configure(healthy, &healthy_request, &healthy_active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(healthy_active.width == 640U && healthy_active.frame_rate_numerator == 30U);
    CHECK(fake.allocated_by_fd[1] == 0U && fake.allocated_by_fd[2] == 4U);
    CHECK(camstream_camera_configure(faulted, &faulted_request, &faulted_active) ==
            CAMSTREAM_CAMERA_STATUS_INVALID_STATE);

    CHECK(camstream_camera_close(faulted) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(faulted, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_configure(faulted, &faulted_request, &faulted_active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(faulted_active.width == 640U && faulted_active.frame_rate_numerator == 30U);
    CHECK(camstream_camera_close(healthy) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(faulted) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(healthy);
    camstream_camera_destroy(faulted);
    CHECK(fake.invalid_call == 0 && fake.open_calls == 3 && fake.close_calls == 3);
    return 0;
}

static int test_configuration_suite(void)
{
    CHECK(test_configuration_success() == 0);
    CHECK(test_buffer_count(CONFIG_REQBUFS_FEWER, 2U) == 0);
    CHECK(test_buffer_count(CONFIG_REQBUFS_MORE, 6U) == 0);
    CHECK(test_zero_address_mapping() == 0);
    CHECK(test_driver_adjustment() == 0);
    CHECK(test_request_validation() == 0);
    CHECK(test_oversized_structures() == 0);
    CHECK(test_configuration_failure(CONFIG_G_FMT_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 0, "VIDIOC_G_FMT") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_G_PARM_EIO_PRE, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 0, "VIDIOC_G_PARM") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_G_PARM_UNSUPPORTED, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 0, "frame interval") == 0);
    CHECK(test_configuration_failure(CONFIG_S_FMT_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_S_FMT") == 0);
    CHECK(test_configuration_failure(CONFIG_S_FMT_MJPEG, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 2, "unsupported") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_S_FMT_BAD_LAYOUT, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "buffer layout") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_S_FMT_BAD_SIZE, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "buffer layout") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_S_FMT_ZERO_WIDTH, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "buffer layout") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_S_FMT_INTERLACED, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "buffer layout") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_S_FMT_OVERFLOW_LAYOUT, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "buffer layout") == 0);
    CHECK(test_configuration_failure(CONFIG_S_PARM_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 2, "VIDIOC_S_PARM") ==
            0);
    CHECK(test_configuration_failure(
                  CONFIG_S_PARM_UNSUPPORTED, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 2, "VIDIOC_S_PARM") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_G_PARM_EIO_POST, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 2, "VIDIOC_G_PARM") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_G_PARM_ZERO_POST, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "confirmed frame interval") == 0);
    CHECK(test_configuration_failure(CONFIG_G_PARM_ZERO_DENOMINATOR_POST, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2,
                  "confirmed frame interval") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_REQBUFS_ZERO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 2, "no MMAP buffers") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_REQBUFS_EINVAL, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 2, "VIDIOC_REQBUFS") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_REQBUFS_ENOMEM, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 2, "VIDIOC_REQBUFS") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_REQBUFS_EBUSY, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 2, "VIDIOC_REQBUFS") == 0);
    CHECK(test_configuration_failure(
                  CONFIG_REQBUFS_WRONG_TYPE, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, 2, "invalid buffer queue") == 0);
    CHECK(test_buffer_preparation_failure(
                  CONFIG_QUERYBUF_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "VIDIOC_QUERYBUF", 1, 0) == 0);
    CHECK(test_buffer_preparation_failure(
                  CONFIG_QUERYBUF_BAD_INDEX, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "QUERYBUF metadata", 1, 0) == 0);
    CHECK(test_buffer_preparation_failure(
                  CONFIG_QUERYBUF_BAD_LENGTH, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, "QUERYBUF metadata", 1, 0) == 0);
    CHECK(test_buffer_preparation_failure(CONFIG_MMAP_FAIL, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "mmap", 2, 0) == 0);
    CHECK(test_buffer_preparation_failure(
                  CONFIG_QBUF_FAIL, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, "VIDIOC_QBUF", 4, 2) == 0);
    CHECK(test_repeated_cycles_and_instances() == 0);
    CHECK(test_instance_fault_isolation() == 0);
    CHECK(test_buffer_release_failure() == 0);
    CHECK(test_unmap_failure_retains_ownership() == 0);
    CHECK(test_request_cleanup_failure() == 0);
    return 0;
}

static int open_streaming(camstream_camera** camera)
{
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(*camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_start(*camera) == CAMSTREAM_CAMERA_STATUS_OK);
    return 0;
}

static int test_stream_success(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(open_streaming(&camera) == 0);
    CHECK(fake.streamon_calls == 1 && fake.qbuf_calls == 4);
    CHECK(camstream_camera_wait_frame(camera, 20U) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_frame_v1 frame = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame)};
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(frame.frame_token != 0U && frame.sequence_number == 1U);
    CHECK(frame.monotonic_timestamp_ns == UINT64_C(10123000000));
    CHECK(frame.width == 640U && frame.height == 480U && frame.pixel_format == CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV);
    CHECK(frame.plane_count == 1U && frame.planes[0].abi_version == CAMSTREAM_CAMERA_ABI_VERSION_V1);
    CHECK(frame.planes[0].data != NULL && frame.planes[0].allocation_size == 614400U);
    CHECK(frame.planes[0].bytes_used == 614400U && frame.planes[0].stride == 1280U);
    CHECK(fake.queued_by_fd[1] == 3U);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.queued_by_fd[1] == 4U && fake.qbuf_calls == 5);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.streamoff_calls == 1 && fake.close_calls == 1 && fake.mapped_by_fd[1] == 0U);
    CHECK(fake.invalid_call == 0 && fake.close_with_mappings == 0);
    return 0;
}

static int test_stream_failures(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    camstream_camera_stream_config_v1 requested;
    CHECK(open_mode(&camera, &requested) == 0);
    camstream_camera_stream_config_v1 active = empty_configuration();
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    fake.stream_case = STREAM_ON_FAIL;
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(fake.streamon_calls == 1 && fake.streamoff_calls == 1);
    CHECK(camstream_camera_wait_frame(camera, 1U) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);

    reset_fake(FAKE_OK);
    CHECK(open_streaming(&camera) == 0);
    fake.stream_case = STREAM_POLL_TIMEOUT;
    CHECK(camstream_camera_wait_frame(camera, 1U) == CAMSTREAM_CAMERA_STATUS_TIMEOUT);
    fake.stream_case = STREAM_POLL_ERROR;
    CHECK(camstream_camera_wait_frame(camera, 1U) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    fake.stream_case = STREAM_POLL_HUP;
    CHECK(camstream_camera_wait_frame(camera, 1U) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    fake.poll_calls = 0;
    fake.stream_case = STREAM_POLL_EINTR;
    CHECK(camstream_camera_wait_frame(camera, 20U) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.poll_calls == 2);
    camstream_camera_frame_v1 frame = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame)};
    fake.stream_case = STREAM_DQBUF_EAGAIN;
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_TIMEOUT);
    fake.stream_case = STREAM_DQBUF_EIO;
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    fake.stream_case = STREAM_DQBUF_UNKNOWN_CLOCK;
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(frame.monotonic_timestamp_ns != UINT64_C(10123000000));
    fake.stream_case = STREAM_RELEASE_QBUF_FAIL;
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    fake.stream_case = STREAM_NORMAL;
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    fake.stream_case = STREAM_OFF_FAIL;
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    fake.stream_case = STREAM_NORMAL;
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0 && fake.close_with_mappings == 0);
    return 0;
}

static int test_recoverable_frame_error(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(open_streaming(&camera) == 0);
    fake.stream_case = STREAM_DQBUF_FLAG_ERROR;
    camstream_camera_frame_v1 frame = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame), .frame_token = 77U};
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_TIMEOUT);
    CHECK(frame.frame_token == 77U);
    CHECK(fake.dqbuf_calls == 1 && fake.qbuf_calls == 5 && fake.queued_by_fd[1] == 4U);
    fake.stream_case = STREAM_NORMAL;
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(frame.frame_token != 0U && frame.frame_token != 77U);
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0 && fake.mapped_by_fd[1] == 0U);

    reset_fake(FAKE_OK);
    CHECK(open_streaming(&camera) == 0);
    fake.stream_case = STREAM_DQBUF_FLAG_ERROR_QBUF_FAIL;
    frame = (camstream_camera_frame_v1){
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame), .frame_token = 77U};
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    CHECK(frame.frame_token == 77U && fake.qbuf_calls == 5 && fake.queued_by_fd[1] == 3U);
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0 && fake.mapped_by_fd[1] == 0U);
    return 0;
}

static int test_bad_frame_metadata(streaming_scenario scenario)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(open_streaming(&camera) == 0);
    fake.stream_case = scenario;
    camstream_camera_frame_v1 frame = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame), .frame_token = 77U};
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR);
    CHECK(frame.frame_token == 77U);
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0 && fake.mapped_by_fd[1] == 0U);
    return 0;
}

static int test_stream_instance_isolation(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* first = NULL;
    camstream_camera* second = NULL;
    CHECK(open_streaming(&first) == 0);
    CHECK(open_streaming(&second) == 0);
    camstream_camera_frame_v1 a = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(a)};
    camstream_camera_frame_v1 b = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(b)};
    CHECK(camstream_camera_acquire_frame(first, &a) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_acquire_frame(second, &b) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(a.frame_token != b.frame_token);
    CHECK(camstream_camera_release_frame(second, a.frame_token) == CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT);
    CHECK(camstream_camera_release_frame(first, a.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_release_frame(second, b.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_stop(first) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_stop(second) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(first) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(second) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(first);
    camstream_camera_destroy(second);
    CHECK(fake.invalid_call == 0 && fake.mapped_by_fd[1] == 0U && fake.mapped_by_fd[2] == 0U);
    return 0;
}

static int test_duplicate_dequeue_retains_owner(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(open_streaming(&camera) == 0);
    camstream_camera_frame_v1 frame = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame)};
    CHECK(camstream_camera_acquire_frame(camera, &frame) == CAMSTREAM_CAMERA_STATUS_OK);
    fake.stream_case = STREAM_DQBUF_DUPLICATE;
    camstream_camera_frame_v1 duplicate = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(duplicate), .frame_token = 77U};
    CHECK(camstream_camera_acquire_frame(camera, &duplicate) == CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR);
    CHECK(duplicate.frame_token == 77U);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
    fake.stream_case = STREAM_NORMAL;
    CHECK(camstream_camera_release_frame(camera, frame.frame_token) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.invalid_call == 0);
    return 0;
}

static int test_destroy_after_streamoff_failure(void)
{
    reset_fake(FAKE_OK);
    camstream_camera* camera = NULL;
    CHECK(open_streaming(&camera) == 0);
    fake.stream_case = STREAM_OFF_FAIL;
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    camstream_camera_destroy(camera);
    CHECK(fake.streamoff_calls >= 2 && fake.close_calls == 1);
    CHECK(fake.mapped_by_fd[1] == 0U && fake.close_with_mappings == 1 && fake.invalid_call == 0);
    return 0;
}

static int test_streaming_suite(void)
{
    CHECK(test_stream_success() == 0);
    CHECK(test_stream_failures() == 0);
    CHECK(test_recoverable_frame_error() == 0);
    CHECK(test_bad_frame_metadata(STREAM_DQBUF_BAD_INDEX) == 0);
    CHECK(test_bad_frame_metadata(STREAM_DQBUF_BAD_BYTES) == 0);
    CHECK(test_bad_frame_metadata(STREAM_DQBUF_BAD_TYPE) == 0);
    CHECK(test_bad_frame_metadata(STREAM_DQBUF_BAD_FIELD) == 0);
    CHECK(test_stream_instance_isolation() == 0);
    CHECK(test_duplicate_dequeue_retains_owner() == 0);
    CHECK(test_destroy_after_streamoff_failure() == 0);
    return 0;
}

int main(int argc, char* argv[])
{
    const bool configuration_test = argc == 3 && strcmp(argv[2], "--configuration") == 0;
    const bool streaming_test = argc == 3 && strcmp(argv[2], "--streaming") == 0;
    if (argc != 2 && !configuration_test && !streaming_test)
    {
        return 2;
    }
    reset_fake(FAKE_OK);
    CHECK(camstream_camera_hal_load_backend(argv[1]) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.open_calls == 0 && fake.close_calls == 0 && fake.ioctl_calls == 0 && fake.invalid_call == 0);

    if (streaming_test)
    {
        CHECK(test_streaming_suite() == 0);
    }
    else if (configuration_test)
    {
        CHECK(test_configuration_suite() == 0);
    }
    else
    {
        CHECK(test_success() == 0);
        CHECK(test_open_failure(FAKE_OPEN_ENODEV, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 0, "open failed") == 0);
        CHECK(test_open_failure(FAKE_QUERYCAP_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_QUERYCAP") == 0);
        CHECK(test_open_failure(FAKE_NO_CAPTURE, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "single-planar") == 0);
        CHECK(test_open_failure(FAKE_NO_STREAMING, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "streaming I/O") == 0);
        CHECK(test_open_failure(FAKE_DEVICE_CAPS_OVERRIDE, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "single-planar") ==
                0);
        CHECK(test_open_failure(FAKE_ENUM_FMT_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_ENUM_FMT") == 0);
        CHECK(test_open_failure(FAKE_NO_MODES, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "no discrete YUYV") == 0);
        CHECK(test_open_failure(
                      FAKE_ENUM_SIZE_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_ENUM_FRAMESIZES") == 0);
        CHECK(test_open_failure(FAKE_ENUM_INTERVAL_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1,
                      "VIDIOC_ENUM_FRAMEINTERVALS") == 0);
        CHECK(test_eintr_and_close_failure() == 0);
        CHECK(test_instance_isolation() == 0);
        CHECK(test_invalid_source() == 0);
    }

    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    puts(streaming_test          ? "V4L2 M3-E streaming regression: PASS"
            : configuration_test ? "V4L2 P2-A configuration regression: PASS"
                                 : "V4L2 Phase-1 C regression: PASS");
    return 0;
}
