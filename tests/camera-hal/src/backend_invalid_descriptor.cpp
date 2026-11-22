#include <camstream/camera/camera_backend.h>

namespace
{

__attribute__((constructor)) static void register_invalid_backend() noexcept
{
    camstream_camera_binding_v2* binding = nullptr;
    if (camstream_camera_hal_get_binding_slot_v2(&binding, sizeof(camstream_camera_binding_v2),
                sizeof(camstream_camera_ops_v2)) == CAMSTREAM_CAMERA_STATUS_OK)
    {
        binding->control_funcs->struct_size = sizeof(camstream_camera_ops_v2);
        binding->backend_name = "invalid-test-backend";
        binding->struct_size = sizeof(*binding);
        binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2 + 1U;
    }
}

} // namespace
