#include <camstream/camera/camera_backend.h>

#include <stddef.h>

#ifndef CAMSTREAM_BINDING_FAILURE_MODE
#error CAMSTREAM_BINDING_FAILURE_MODE must select a binding fault
#endif

__attribute__((constructor)) static void inject_invalid_binding(void)
{
    camstream_camera_binding_v2* binding = NULL;
#if CAMSTREAM_BINDING_FAILURE_MODE == 5
    (void)camstream_camera_hal_get_binding_slot_v2(
            &binding, sizeof(camstream_camera_binding_v2) + 1U, sizeof(camstream_camera_ops_v2));
    return;
#endif
    if (camstream_camera_hal_get_binding_slot_v2(&binding, sizeof(camstream_camera_binding_v2),
                sizeof(camstream_camera_ops_v2)) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        return;
    }

    binding->backend_name = "binding-fault-test";
    binding->struct_size = sizeof(*binding);
    binding->control_funcs->struct_size = sizeof(*binding->control_funcs);

#if CAMSTREAM_BINDING_FAILURE_MODE == 1
    /* Required callbacks remain null. */
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
#elif CAMSTREAM_BINDING_FAILURE_MODE == 2
    binding->struct_size = sizeof(*binding) - 1U;
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
#elif CAMSTREAM_BINDING_FAILURE_MODE == 3
    binding->control_funcs->struct_size = sizeof(*binding->control_funcs) - 1U;
    binding->abi_version = CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2;
#elif CAMSTREAM_BINDING_FAILURE_MODE == 4
    /* A claimed slot without a finalized version must never become active. */
#elif CAMSTREAM_BINDING_FAILURE_MODE == 5
    /* The hook rejected this backend before granting writable storage. */
#else
#error Unsupported binding failure mode
#endif
}
