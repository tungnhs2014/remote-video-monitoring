#define _POSIX_C_SOURCE 200809L
#include "v4l2_internal.h"

static const char backend_name[] = "v4l2";

__attribute__((constructor)) static void inject_v4l2_backend(void)
{
    camstream_camera_binding_v2* binding = NULL;
    if (camstream_camera_hal_get_binding_slot_v2(&binding, sizeof(camstream_camera_binding_v2),
                sizeof(camstream_camera_ops_v2)) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return;
    }

    *binding->control_funcs = (camstream_camera_ops_v2){
            .struct_size = sizeof(camstream_camera_ops_v2),
            .camera_create = v4l2_create,
            .camera_destroy = v4l2_destroy,
            .camera_open = v4l2_open,
            .camera_close = v4l2_close,
            .camera_get_capabilities = v4l2_get_capabilities,
            .camera_get_stream_configuration = v4l2_get_stream_configuration,
            .camera_configure = v4l2_configure,
            .camera_start = v4l2_start,
            .camera_wait_frame = v4l2_wait_frame,
            .camera_acquire_frame = v4l2_acquire_frame,
            .camera_release_frame = v4l2_release_frame,
            .camera_stop = v4l2_stop,
            .camera_get_last_error = v4l2_get_last_error,
    };
    binding->backend_name = backend_name;
    binding->struct_size = sizeof(*binding);
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
}
