#include <camstream/camera/camera_backend.h>

#include <cstdint>
#include <cstdio>
#include <new>

extern "C" int camstream_test_blocking_backend_enter_and_wait(void);
extern "C" void camstream_test_blocking_backend_destroyed(void);

namespace
{

constexpr char kBackendName[] = "blocking-create-test";

struct BlockingCamera
{
};

camstream_camera_status_t unsupported_operation() noexcept
{
    return CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
}

extern "C"
{

    static camstream_camera_status_t blocking_create(camstream_camera_instance** instance)
    {
        if (instance == nullptr)
        {
            return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
        }
        *instance = nullptr;
        BlockingCamera* const camera = new (std::nothrow) BlockingCamera();
        if (camera == nullptr)
        {
            return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        }
        *instance = reinterpret_cast<camstream_camera_instance*>(camera);
        try
        {
            return camstream_test_blocking_backend_enter_and_wait() != 0 ? CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR
                                                                         : CAMSTREAM_CAMERA_STATUS_OK;
        }
        catch (...)
        {
            return CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
        }
    }

    static void blocking_destroy(camstream_camera_instance* instance)
    {
        delete reinterpret_cast<BlockingCamera*>(instance);
        camstream_test_blocking_backend_destroyed();
    }

    static camstream_camera_status_t blocking_open(camstream_camera_instance* instance, const char* source_identifier)
    {
        static_cast<void>(instance);
        static_cast<void>(source_identifier);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_close(camstream_camera_instance* instance)
    {
        static_cast<void>(instance);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_get_capabilities(
            camstream_camera_instance* instance, camstream_camera_capabilities_v1* capabilities)
    {
        static_cast<void>(instance);
        static_cast<void>(capabilities);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_get_stream_configuration(
            camstream_camera_instance* instance, std::uint32_t index, camstream_camera_stream_config_v1* configuration)
    {
        static_cast<void>(instance);
        static_cast<void>(index);
        static_cast<void>(configuration);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_configure(camstream_camera_instance* instance,
            const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active)
    {
        static_cast<void>(instance);
        static_cast<void>(requested);
        static_cast<void>(active);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_start(camstream_camera_instance* instance)
    {
        static_cast<void>(instance);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_wait_frame(camstream_camera_instance* instance, std::uint32_t timeout_ms)
    {
        static_cast<void>(instance);
        static_cast<void>(timeout_ms);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_acquire_frame(
            camstream_camera_instance* instance, camstream_camera_frame_v1* frame)
    {
        static_cast<void>(instance);
        static_cast<void>(frame);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_release_frame(
            camstream_camera_instance* instance, std::uint64_t frame_token)
    {
        static_cast<void>(instance);
        static_cast<void>(frame_token);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_stop(camstream_camera_instance* instance)
    {
        static_cast<void>(instance);
        return unsupported_operation();
    }

    static camstream_camera_status_t blocking_get_last_error(
            camstream_camera_instance* instance, char* buffer, std::uint32_t buffer_size)
    {
        static_cast<void>(instance);
        if (buffer == nullptr || buffer_size == 0U)
        {
            return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
        }
        const int result = std::snprintf(buffer, buffer_size, "%s", "blocking create test backend failure");
        return result < 0 ? CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR : CAMSTREAM_CAMERA_STATUS_OK;
    }

} // extern "C"

} // namespace

__attribute__((constructor)) static void register_blocking_backend() noexcept
{
    camstream_camera_binding_v2* binding = nullptr;
    if (camstream_camera_hal_get_binding_slot_v2(&binding, sizeof(camstream_camera_binding_v2),
                sizeof(camstream_camera_ops_v2)) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return;
    }
    *binding->control_funcs = {sizeof(camstream_camera_ops_v2), blocking_create, blocking_destroy, blocking_open,
            blocking_close, blocking_get_capabilities, blocking_get_stream_configuration, blocking_configure,
            blocking_start, blocking_wait_frame, blocking_acquire_frame, blocking_release_frame, blocking_stop,
            blocking_get_last_error, {}};
    binding->backend_name = kBackendName;
    binding->struct_size = sizeof(*binding);
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
}
