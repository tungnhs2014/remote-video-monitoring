#ifndef CAMSTREAM_CAMERA_CAMERA_BACKEND_H
#define CAMSTREAM_CAMERA_CAMERA_BACKEND_H

/**
 * @file camera_backend.h
 * @brief Versioned constructor-injection SPI for Camera HAL backends.
 *
 * A backend owns every opaque instance and frame payload it creates. Each
 * instance is independent, and no callback may allow a C++ exception to cross
 * this C boundary.
 */

#include <camstream/camera/camera_hal.h>

/* Clang-format 14 mis-indents declarations when C++ linkage braces are conditional. */
#ifdef __cplusplus
extern "C"
{
#endif
    // clang-format off

/** @brief Opaque backend instance owned by the HAL after create(). */
typedef struct camstream_camera_instance camstream_camera_instance;

/**
 * @brief Creates one opaque instance; ownership passes to the HAL.
 *
 * A failed create may return a partially initialized, non-NULL instance.
 * The HAL calls destroy on that instance before releasing its module-create
 * reservation, so destroy must accept partial initialization.
 */
typedef camstream_camera_status_t (*camstream_camera_create_v1_fn)(camstream_camera_instance** instance);

/**
 * @brief Destroys an instance after all other callbacks have ceased.
 *
 * The backend must reclaim remaining resources even after partial
 * initialization or a failed best-effort stop/close sequence.
 */
typedef void (*camstream_camera_destroy_v1_fn)(camstream_camera_instance* instance);

/** @brief Opens one backend-specific source on a created instance. */
typedef camstream_camera_status_t (*camstream_camera_open_v1_fn)(camstream_camera_instance* instance, const char* source_identifier);

/** @brief Closes a stopped or non-started source. */
typedef camstream_camera_status_t (*camstream_camera_close_v1_fn)(camstream_camera_instance* instance);

/** @brief Reports bounded capabilities for an open source. */
typedef camstream_camera_status_t (*camstream_camera_get_capabilities_v1_fn)(
        camstream_camera_instance* instance, camstream_camera_capabilities_v1* capabilities);

/** @brief Returns one supported configuration by zero-based index. */
typedef camstream_camera_status_t (*camstream_camera_get_stream_configuration_v1_fn)(
        camstream_camera_instance* instance, uint32_t index, camstream_camera_stream_config_v1* configuration);

/** @brief Applies a request and reports the backend's active configuration. */
typedef camstream_camera_status_t (*camstream_camera_configure_v1_fn)(
        camstream_camera_instance* instance, const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active);

/** @brief Starts delivery for a configured instance. */
typedef camstream_camera_status_t (*camstream_camera_start_v1_fn)(camstream_camera_instance* instance);

/** @brief Waits for availability and may return the normal TIMEOUT status. */
typedef camstream_camera_status_t (*camstream_camera_wait_frame_v1_fn)(camstream_camera_instance* instance, uint32_t timeout_ms);

/** @brief Transfers temporary access to one backend-owned frame. */
typedef camstream_camera_status_t (*camstream_camera_acquire_frame_v1_fn)(
        camstream_camera_instance* instance, camstream_camera_frame_v1* frame);

/** @brief Returns an outstanding frame token and invalidates its plane views. */
typedef camstream_camera_status_t (*camstream_camera_release_frame_v1_fn)(camstream_camera_instance* instance, uint64_t frame_token);

/** @brief Stops delivery after every outstanding frame has been released. */
typedef camstream_camera_status_t (*camstream_camera_stop_v1_fn)(camstream_camera_instance* instance);

/** @brief Copies the last diagnostic into caller-owned bounded storage. */
typedef camstream_camera_status_t (*camstream_camera_get_last_error_v1_fn)(
        camstream_camera_instance* instance, char* buffer, uint32_t buffer_size);

#define CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2 2U

/** @brief Mandatory backend callbacks. The HAL owns this table; the backend fills it during its constructor. */
typedef struct camstream_camera_ops_v2
{
    uint32_t struct_size;
    camstream_camera_create_v1_fn camera_create;
    camstream_camera_destroy_v1_fn camera_destroy;
    camstream_camera_open_v1_fn camera_open;
    camstream_camera_close_v1_fn camera_close;
    camstream_camera_get_capabilities_v1_fn camera_get_capabilities;
    camstream_camera_get_stream_configuration_v1_fn camera_get_stream_configuration;
    camstream_camera_configure_v1_fn camera_configure;
    camstream_camera_start_v1_fn camera_start;
    camstream_camera_wait_frame_v1_fn camera_wait_frame;
    camstream_camera_acquire_frame_v1_fn camera_acquire_frame;
    camstream_camera_release_frame_v1_fn camera_release_frame;
    camstream_camera_stop_v1_fn camera_stop;
    camstream_camera_get_last_error_v1_fn camera_get_last_error;
    uint32_t reserved[8];
} camstream_camera_ops_v2;

/**
 * @brief HAL-owned backend binding. The staging name has static module lifetime.
 *
 * During loading, the backend fills control_funcs first, then sets metadata and
 * abi_version last. The HAL validates and copies the binding before publication.
 * The active binding uses a HAL-owned copy of the name. The backend must not
 * retain or modify the staging address after its constructor.
 */
typedef struct camstream_camera_binding_v2
{
    uint32_t abi_version;
    uint32_t struct_size;
    const char* backend_name;
    camstream_camera_ops_v2* control_funcs;
    uint32_t reserved[8];
} camstream_camera_binding_v2;

/**
 * @brief Claim the HAL-owned staging slot from the backend ELF constructor.
 * @param[out] binding Receives the temporary slot, valid only during this constructor.
 * @param[in] binding_size Backend's compile-time sizeof(camstream_camera_binding_v2).
 * @param[in] operations_size Backend's compile-time sizeof(camstream_camera_ops_v2).
 * @return OK once per load on the loader thread; INVALID_ARGUMENT for a null
 * output; INVALID_STATE outside Loading or on duplicate claim; RESOURCE_ERROR
 * if the runtime is faulted; NOT_SUPPORTED for a size mismatch.
 *
 * The constructor performs no device I/O. A second claim poisons this load.
 * SPI v2 has fixed writable capacities; extensions require a new versioned hook.
 * A backend built for the removed v1 registration hook cannot load with v2 HAL.
 */
camstream_camera_status_t camstream_camera_hal_get_binding_slot_v2(
        camstream_camera_binding_v2** binding, uint32_t binding_size, uint32_t operations_size);

// clang-format on
#ifdef __cplusplus
}
#endif

#endif
