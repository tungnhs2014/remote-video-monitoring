/* A stale v1 module must fail RTLD_NOW symbol resolution against the v2 HAL. */
extern int camstream_camera_hal_register_backend_v1(const void* descriptor);

__attribute__((constructor)) static void inject_legacy_backend(void)
{
    (void)camstream_camera_hal_register_backend_v1(0);
}
