#include <camstream/camera/camera_hal.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Executable interposition keeps the candidate module loaded until process exit. */
int dlclose(void* handle)
{
    (void)handle;
    return -1;
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        return 2;
    }
    if (camstream_camera_hal_load_backend(argv[1]) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return 1;
    }
    if (camstream_camera_hal_unload_backend() != CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR)
    {
        return 1;
    }

    char error[256] = {0};
    if (camstream_camera_hal_get_last_error(error, sizeof(error)) != CAMSTREAM_CAMERA_STATUS_OK ||
            strstr(error, "dlclose failed") == NULL)
    {
        fprintf(stderr, "missing dlclose failure diagnostic: %s\n", error);
        return 1;
    }
    if (camstream_camera_hal_load_backend(argv[1]) != CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR)
    {
        return 1;
    }
    return 0;
}
