#include <camstream/camera/camera_hal.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    STREAMING_FRAME_TARGET = 10U,
    STREAMING_MAX_ATTEMPTS = 40U,
    STREAMING_WAIT_TIMEOUT_MS = 500U
};

static void print_error(camstream_camera* camera, const char* operation, camstream_camera_status_t status)
{
    char error[256] = {0};
    if (camera != NULL)
    {
        (void)camstream_camera_get_last_error(camera, error, sizeof(error));
    }
    else
    {
        (void)camstream_camera_hal_get_last_error(error, sizeof(error));
    }
    fprintf(stderr, "%s failed (%d): %s\n", operation, status, error);
}

int main(int argc, char* argv[])
{
    const bool streaming_smoke = argc == 5 && strcmp(argv[4], "--stream") == 0;
    if (argc != 3 && argc != 4 && !streaming_smoke)
    {
        fprintf(stderr, "usage: %s BACKEND_MODULE DEVICE_NODE [CONFIG_INDEX [--stream]]\n", argv[0]);
        return 2;
    }
    uint32_t selected_index = 0U;
    if (argc >= 4)
    {
        char* end = NULL;
        errno = 0;
        const unsigned long parsed = strtoul(argv[3], &end, 10);
        if (errno != 0 || argv[3][0] == '-' || end == argv[3] || *end != '\0' || parsed > UINT32_MAX)
        {
            fprintf(stderr, "invalid configuration index: %s\n", argv[3]);
            return 2;
        }
        selected_index = (uint32_t)parsed;
    }

    camstream_camera_status_t status = camstream_camera_hal_load_backend(argv[1]);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(NULL, "load backend", status);
        return 1;
    }

    int result = 1;
    camstream_camera* camera = NULL;
    bool started = false;
    uint64_t outstanding_token = 0U;
    uint64_t streaming_bytes = 0U;
    uint64_t first_sequence = 0U;
    uint32_t frames_received = 0U;
    uint32_t streaming_attempts = 0U;
    uint32_t recoverable_timeouts = 0U;
    status = camstream_camera_create(&camera);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(NULL, "create camera", status);
        goto done;
    }
    status = camstream_camera_open(camera, argv[2]);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(camera, "open device", status);
        goto done;
    }

    camstream_camera_capabilities_v1 capabilities = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(capabilities),
    };
    status = camstream_camera_get_capabilities(camera, &capabilities);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(camera, "get capabilities", status);
        goto close_camera;
    }
    printf("discrete configurations: %u\n", capabilities.stream_config_count);
    camstream_camera_stream_config_v1 selected = {0};
    bool have_selected = false;
    for (uint32_t index = 0U; index < capabilities.stream_config_count; ++index)
    {
        camstream_camera_stream_config_v1 configuration = {
                .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                .struct_size = sizeof(configuration),
        };
        status = camstream_camera_get_stream_configuration(camera, index, &configuration);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            print_error(camera, "get configuration", status);
            goto close_camera;
        }
        printf("%u: %s %ux%u fourcc=0x%08x fps=%u/%u\n", index, configuration.source_identifier, configuration.width,
                configuration.height, configuration.pixel_format, configuration.frame_rate_numerator,
                configuration.frame_rate_denominator);
        if (argc >= 4 && index == selected_index)
        {
            selected = configuration;
            have_selected = true;
        }
    }
    if (argc >= 4)
    {
        if (!have_selected)
        {
            fprintf(stderr, "configuration index %u is out of range\n", selected_index);
            goto close_camera;
        }
        camstream_camera_stream_config_v1 active = {
                .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                .struct_size = sizeof(active),
        };
        status = camstream_camera_configure(camera, &selected, &active);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            print_error(camera, "configure device", status);
            goto close_camera;
        }
        printf("active: %s %ux%u fourcc=0x%08x fps=%u/%u\n", active.source_identifier, active.width, active.height,
                active.pixel_format, active.frame_rate_numerator, active.frame_rate_denominator);
        if (streaming_smoke)
        {
            status = camstream_camera_start(camera);
            if (status != CAMSTREAM_CAMERA_STATUS_OK)
            {
                print_error(camera, "start streaming", status);
                goto close_camera;
            }
            started = true;
            /* A recoverable DQBUF frame error is requeued by the backend and reported as TIMEOUT. */
            while (frames_received < STREAMING_FRAME_TARGET && streaming_attempts < STREAMING_MAX_ATTEMPTS)
            {
                ++streaming_attempts;
                status = camstream_camera_wait_frame(camera, STREAMING_WAIT_TIMEOUT_MS);
                if (status == CAMSTREAM_CAMERA_STATUS_TIMEOUT)
                {
                    ++recoverable_timeouts;
                    continue;
                }
                if (status != CAMSTREAM_CAMERA_STATUS_OK)
                {
                    print_error(camera, "wait frame", status);
                    goto close_camera;
                }
                camstream_camera_frame_v1 frame = {
                        .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
                        .struct_size = sizeof(frame),
                };
                status = camstream_camera_acquire_frame(camera, &frame);
                if (status == CAMSTREAM_CAMERA_STATUS_TIMEOUT)
                {
                    ++recoverable_timeouts;
                    continue;
                }
                if (status != CAMSTREAM_CAMERA_STATUS_OK)
                {
                    print_error(camera, "acquire frame", status);
                    goto close_camera;
                }
                outstanding_token = frame.frame_token;
                if (frame.plane_count != 1U || frame.planes[0].data == NULL || frame.planes[0].bytes_used == 0U ||
                        frame.planes[0].bytes_used > frame.planes[0].allocation_size || frame.width != active.width ||
                        frame.height != active.height || frame.pixel_format != active.pixel_format ||
                        UINT64_MAX - streaming_bytes < frame.planes[0].bytes_used)
                {
                    fprintf(stderr, "invalid streaming frame metadata\n");
                    goto close_camera;
                }
                if (frames_received == 0U)
                {
                    first_sequence = frame.sequence_number;
                }
                streaming_bytes += frame.planes[0].bytes_used;
                status = camstream_camera_release_frame(camera, outstanding_token);
                if (status != CAMSTREAM_CAMERA_STATUS_OK)
                {
                    print_error(camera, "release frame", status);
                    goto close_camera;
                }
                outstanding_token = 0U;
                ++frames_received;
            }
            if (frames_received != STREAMING_FRAME_TARGET)
            {
                fprintf(stderr, "streaming: FAIL frames=%u attempts=%u timeouts=%u (retry limit reached)\n",
                        frames_received, streaming_attempts, recoverable_timeouts);
                goto close_camera;
            }
            status = camstream_camera_stop(camera);
            if (status != CAMSTREAM_CAMERA_STATUS_OK)
            {
                print_error(camera, "stop streaming", status);
                goto close_camera;
            }
            started = false;
        }
    }
    result = 0;

close_camera:
    if (outstanding_token != 0U)
    {
        status = camstream_camera_release_frame(camera, outstanding_token);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            print_error(camera, "cleanup release frame", status);
            result = 1;
        }
    }
    if (started)
    {
        status = camstream_camera_stop(camera);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            print_error(camera, "cleanup stop streaming", status);
            result = 1;
        }
        else
        {
            started = false;
        }
    }
    if (!started)
    {
        status = camstream_camera_close(camera);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            print_error(camera, "close device", status);
            result = 1;
        }
    }
done:
    camstream_camera_destroy(camera);
    status = camstream_camera_hal_unload_backend();
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(NULL, "unload backend", status);
        result = 1;
    }
    if (streaming_smoke && result == 0)
    {
        printf("streaming: frames=%u bytes=%llu first_sequence=%llu attempts=%u timeouts=%u\n", frames_received,
                (unsigned long long)streaming_bytes, (unsigned long long)first_sequence, streaming_attempts,
                recoverable_timeouts);
        puts("streaming: PASS");
    }
    return result;
}
