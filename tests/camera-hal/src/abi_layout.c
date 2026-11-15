#include <camstream/camera/camera_backend.h>

#include <stddef.h>

#ifdef __cplusplus
#define CAMSTREAM_LAYOUT_ALIGNOF(type) alignof(type)
#else
#define CAMSTREAM_LAYOUT_ALIGNOF(type) _Alignof(type)
#endif

/* Compile this probe against each header/toolchain and compare its .rodata bytes. */
const size_t camstream_camera_abi_layout[] = {
        sizeof(camstream_camera_status_t),
        CAMSTREAM_LAYOUT_ALIGNOF(camstream_camera_status_t),
        sizeof(camstream_camera_stream_config_v1),
        CAMSTREAM_LAYOUT_ALIGNOF(camstream_camera_stream_config_v1),
        offsetof(camstream_camera_stream_config_v1, source_identifier),
        offsetof(camstream_camera_stream_config_v1, reserved),
        sizeof(camstream_camera_capabilities_v1),
        CAMSTREAM_LAYOUT_ALIGNOF(camstream_camera_capabilities_v1),
        offsetof(camstream_camera_capabilities_v1, reserved),
        sizeof(camstream_camera_plane_v1),
        CAMSTREAM_LAYOUT_ALIGNOF(camstream_camera_plane_v1),
        offsetof(camstream_camera_plane_v1, data),
        offsetof(camstream_camera_plane_v1, reserved),
        sizeof(camstream_camera_frame_v1),
        CAMSTREAM_LAYOUT_ALIGNOF(camstream_camera_frame_v1),
        offsetof(camstream_camera_frame_v1, frame_token),
        offsetof(camstream_camera_frame_v1, planes),
        offsetof(camstream_camera_frame_v1, reserved),
        sizeof(camstream_camera_backend_v1),
        CAMSTREAM_LAYOUT_ALIGNOF(camstream_camera_backend_v1),
        offsetof(camstream_camera_backend_v1, create),
        offsetof(camstream_camera_backend_v1, reserved),
};
