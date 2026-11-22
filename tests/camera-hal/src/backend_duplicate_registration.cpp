#include <camstream/camera/camera_backend.h>

namespace
{

__attribute__((constructor)) static void register_backend_twice() noexcept
{
    camstream_camera_binding_v2* binding = nullptr;
    static_cast<void>(camstream_camera_hal_get_binding_slot_v2(
            &binding, sizeof(camstream_camera_binding_v2), sizeof(camstream_camera_ops_v2)));
    static_cast<void>(camstream_camera_hal_get_binding_slot_v2(
            &binding, sizeof(camstream_camera_binding_v2), sizeof(camstream_camera_ops_v2)));
}

} // namespace
