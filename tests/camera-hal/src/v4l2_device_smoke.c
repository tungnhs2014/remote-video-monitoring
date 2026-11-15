#include <camstream/camera/camera_hal.h>

#include <stdio.h>

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
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s BACKEND_MODULE DEVICE_NODE\n", argv[0]);
        return 2;
    }

    camstream_camera_status_t status = camstream_camera_hal_load_backend(argv[1]);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(NULL, "load backend", status);
        return 1;
    }

    int result = 1;
    camstream_camera* camera = NULL;
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
    }
    result = 0;

close_camera:
    status = camstream_camera_close(camera);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(camera, "close device", status);
        result = 1;
    }
done:
    camstream_camera_destroy(camera);
    status = camstream_camera_hal_unload_backend();
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        print_error(NULL, "unload backend", status);
        result = 1;
    }
    return result;
}
