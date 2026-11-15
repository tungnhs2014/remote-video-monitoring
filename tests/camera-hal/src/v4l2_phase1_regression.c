#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_hal.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

static struct fake_state
{
    fake_scenario scenario;
    int open_calls;
    int close_calls;
    int ioctl_calls;
    int querycap_calls;
    int invalid_call;
    int close_fails;
} fake;

static void reset_fake(fake_scenario scenario)
{
    memset(&fake, 0, sizeof(fake));
    fake.scenario = scenario;
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
    if (fd < 101 || argument == NULL)
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
    fake.invalid_call = 1;
    return fail_with(EINVAL);
}

int camstream_test_v4l2_close(int fd)
{
    ++fake.close_calls;
    if (fd < 101)
    {
        fake.invalid_call = 1;
        return fail_with(EBADF);
    }
    if (fake.close_fails)
    {
        return fail_with(EIO);
    }
    return 0;
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

static int test_retry_and_close_failure(void)
{
    reset_fake(FAKE_QUERYCAP_EINTR);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, "/dev/video-test") == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.querycap_calls == 2);
    fake.close_fails = 1;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR);
    fake.close_fails = 0;
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(fake.close_calls == 1 && fake.invalid_call == 0);
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

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        return 2;
    }
    reset_fake(FAKE_OK);
    CHECK(camstream_camera_hal_load_backend(argv[1]) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(fake.open_calls == 0 && fake.close_calls == 0 && fake.ioctl_calls == 0 && fake.invalid_call == 0);

    CHECK(test_success() == 0);
    CHECK(test_open_failure(FAKE_OPEN_ENODEV, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 0, "open failed") == 0);
    CHECK(test_open_failure(FAKE_QUERYCAP_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_QUERYCAP") == 0);
    CHECK(test_open_failure(FAKE_NO_CAPTURE, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "single-planar") == 0);
    CHECK(test_open_failure(FAKE_NO_STREAMING, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "streaming I/O") == 0);
    CHECK(test_open_failure(FAKE_DEVICE_CAPS_OVERRIDE, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "single-planar") == 0);
    CHECK(test_open_failure(FAKE_ENUM_FMT_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_ENUM_FMT") == 0);
    CHECK(test_open_failure(FAKE_NO_MODES, CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED, 1, "no discrete YUYV") == 0);
    CHECK(test_open_failure(FAKE_ENUM_SIZE_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1, "VIDIOC_ENUM_FRAMESIZES") ==
            0);
    CHECK(test_open_failure(FAKE_ENUM_INTERVAL_EIO, CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR, 1,
                  "VIDIOC_ENUM_FRAMEINTERVALS") == 0);
    CHECK(test_retry_and_close_failure() == 0);
    CHECK(test_instance_isolation() == 0);
    CHECK(test_invalid_source() == 0);

    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);
    puts("V4L2 Phase-1 C regression: PASS");
    return 0;
}
