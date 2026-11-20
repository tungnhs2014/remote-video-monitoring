#define _POSIX_C_SOURCE 200809L
#include "v4l2_internal.h"

static const char backend_name[] = "v4l2";

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
