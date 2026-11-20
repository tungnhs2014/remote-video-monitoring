#include <camstream/camera/camera_hal.h>

#include "camera_backend_runtime.h"
#include "camera_core.h"

camstream_camera_status_t camstream_camera_hal_load_backend(const char* backend_path)
{
    return camstream_runtime_load_backend(backend_path);
}

camstream_camera_status_t camstream_camera_hal_unload_backend(void)
{
    return camstream_runtime_unload_backend();
}

camstream_camera_status_t camstream_camera_hal_get_last_error(char* buffer, uint32_t buffer_size)
{
    return camstream_runtime_copy_error(buffer, buffer_size);
}

camstream_camera_status_t camstream_camera_create(camstream_camera** camera)
{
    return camera_core_create(camera);
}

void camstream_camera_destroy(camstream_camera* camera)
{
    camera_core_destroy(camera);
}

camstream_camera_status_t camstream_camera_get_backend_name(
        camstream_camera* camera, char* buffer, uint32_t buffer_size)
{
    return camera_core_get_backend_name(camera, buffer, buffer_size);
}

camstream_camera_status_t camstream_camera_get_backend_abi_version(camstream_camera* camera, uint32_t* abi_version)
{
    return camera_core_get_backend_abi_version(camera, abi_version);
}

camstream_camera_status_t camstream_camera_open(camstream_camera* camera, const char* source_identifier)
{
    return camera_core_open(camera, source_identifier);
}

camstream_camera_status_t camstream_camera_close(camstream_camera* camera)
{
    return camera_core_close(camera);
}

camstream_camera_status_t camstream_camera_get_capabilities(
        camstream_camera* camera, camstream_camera_capabilities_v1* capabilities)
{
    return camera_core_get_capabilities(camera, capabilities);
}

camstream_camera_status_t camstream_camera_get_stream_configuration(
        camstream_camera* camera, uint32_t index, camstream_camera_stream_config_v1* configuration)
{
    return camera_core_get_stream_configuration(camera, index, configuration);
}

camstream_camera_status_t camstream_camera_configure(camstream_camera* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
{
    return camera_core_configure(camera, requested, active);
}

camstream_camera_status_t camstream_camera_start(camstream_camera* camera)
{
    return camera_core_start(camera);
}

camstream_camera_status_t camstream_camera_wait_frame(camstream_camera* camera, uint32_t timeout_ms)
{
    return camera_core_wait_frame(camera, timeout_ms);
}

camstream_camera_status_t camstream_camera_acquire_frame(camstream_camera* camera, camstream_camera_frame_v1* frame)
{
    return camera_core_acquire_frame(camera, frame);
}

camstream_camera_status_t camstream_camera_release_frame(camstream_camera* camera, uint64_t frame_token)
{
    return camera_core_release_frame(camera, frame_token);
}

camstream_camera_status_t camstream_camera_stop(camstream_camera* camera)
{
    return camera_core_stop(camera);
}

camstream_camera_status_t camstream_camera_get_last_error(camstream_camera* camera, char* buffer, uint32_t buffer_size)
{
    return camera_core_get_last_error(camera, buffer, buffer_size);
}
