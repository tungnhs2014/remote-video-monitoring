#ifndef CAMSTREAM_CAMERA_CORE_H
#define CAMSTREAM_CAMERA_CORE_H

#include <camstream/camera/camera_hal.h>

camstream_camera_status_t camera_core_create(camstream_camera** camera);

void camera_core_destroy(camstream_camera* camera);

camstream_camera_status_t camera_core_get_backend_name(camstream_camera* camera, char* buffer, uint32_t buffer_size);

camstream_camera_status_t camera_core_get_backend_abi_version(camstream_camera* camera, uint32_t* abi_version);

camstream_camera_status_t camera_core_open(camstream_camera* camera, const char* source_identifier);

camstream_camera_status_t camera_core_close(camstream_camera* camera);

camstream_camera_status_t camera_core_get_capabilities(camstream_camera* camera, camstream_camera_capabilities_v1* capabilities);

camstream_camera_status_t camera_core_get_stream_configuration(
        camstream_camera* camera, uint32_t index, camstream_camera_stream_config_v1* configuration);

camstream_camera_status_t camera_core_configure(
        camstream_camera* camera, const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active);

camstream_camera_status_t camera_core_start(camstream_camera* camera);

camstream_camera_status_t camera_core_wait_frame(camstream_camera* camera, uint32_t timeout_ms);

camstream_camera_status_t camera_core_acquire_frame(camstream_camera* camera, camstream_camera_frame_v1* frame);

camstream_camera_status_t camera_core_release_frame(camstream_camera* camera, uint64_t frame_token);

camstream_camera_status_t camera_core_stop(camstream_camera* camera);

camstream_camera_status_t camera_core_get_last_error(camstream_camera* camera, char* buffer, uint32_t buffer_size);

#endif
