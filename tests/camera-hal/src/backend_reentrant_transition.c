#include <camstream/camera/camera_hal.h>

/* These symbols come from the test executable, which exports them to dlopen modules. */
extern const char* camstream_test_reentrant_path(void);
extern void camstream_test_hold_constructor(void);
extern void camstream_test_record_reentrant_status(
        int phase, camstream_camera_status_t load_status, camstream_camera_status_t unload_status);

static void try_recursive_transition(int phase)
{
    const camstream_camera_status_t load_status = camstream_camera_hal_load_backend(camstream_test_reentrant_path());
    const camstream_camera_status_t unload_status = camstream_camera_hal_unload_backend();
    camstream_test_record_reentrant_status(phase, load_status, unload_status);
}

__attribute__((constructor)) static void reentrant_constructor(void)
{
    try_recursive_transition(0);
    camstream_test_hold_constructor();
}

__attribute__((destructor)) static void reentrant_destructor(void)
{
    try_recursive_transition(1);
}
