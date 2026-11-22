#ifndef CAMSTREAM_CAMERA_BACKEND_RUNTIME_H
#define CAMSTREAM_CAMERA_BACKEND_RUNTIME_H

#include <camstream/camera/camera_backend.h>

/* Keep the HAL's internal coordination symbols out of its public ELF ABI. */
#define CAMSTREAM_RUNTIME_PRIVATE __attribute__((visibility("hidden")))

CAMSTREAM_RUNTIME_PRIVATE camstream_camera_status_t camstream_runtime_load_backend(const char* backend_path);

CAMSTREAM_RUNTIME_PRIVATE camstream_camera_status_t camstream_runtime_unload_backend(void);

CAMSTREAM_RUNTIME_PRIVATE camstream_camera_status_t camstream_runtime_copy_error(char* buffer, uint32_t buffer_size);

CAMSTREAM_RUNTIME_PRIVATE void camstream_runtime_set_error(const char* error);

CAMSTREAM_RUNTIME_PRIVATE camstream_camera_status_t camstream_runtime_reserve_create(const camstream_camera_binding_v2** binding);

CAMSTREAM_RUNTIME_PRIVATE camstream_camera_status_t camstream_runtime_commit_create(void);

CAMSTREAM_RUNTIME_PRIVATE camstream_camera_status_t camstream_runtime_cancel_create(void);

CAMSTREAM_RUNTIME_PRIVATE void camstream_runtime_record_destroy(void);

#undef CAMSTREAM_RUNTIME_PRIVATE

#endif
