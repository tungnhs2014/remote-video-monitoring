#include "camera_backend_runtime.h"

#define CAMSTREAM_LOG_MODULE "camera-hal-runtime"
#include <camstream/common/log.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum runtime_state
{
    RUNTIME_UNLOADED,
    RUNTIME_LOADING,
    RUNTIME_LOADED,
    RUNTIME_UNLOADING,
    RUNTIME_FAULTED
} runtime_state;

typedef struct backend_runtime
{
    pthread_mutex_t mutex;
    pthread_cond_t state_changed;
    runtime_state state;
    void* module_handle;
    camstream_camera_binding_v2 staging;
    camstream_camera_ops_v2 staging_ops;
    camstream_camera_binding_v2 active;
    camstream_camera_ops_v2 active_ops;
    pthread_t transition_thread;
    char* backend_name;
    char* backend_path;
    char* last_error;
    camstream_camera_status_t registration_status;
    bool registration_attempted;
    uint32_t client_count;
    uint32_t live_instance_count;
    uint32_t create_reservation_count;
} backend_runtime;

/* This shared library is the single process-wide owner of module and token state. */
static backend_runtime runtime = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .state_changed = PTHREAD_COND_INITIALIZER,
        .state = RUNTIME_UNLOADED,
        .registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR,
};

/* A failed primitive leaves ownership of the runtime state indeterminate. */
static void check_pthread(int result)
{
    if (result != 0)
    {
        abort();
    }
}

static void runtime_lock(void)
{
    check_pthread(pthread_mutex_lock(&runtime.mutex));
}

static void runtime_unlock(void)
{
    check_pthread(pthread_mutex_unlock(&runtime.mutex));
}

static void runtime_notify(void)
{
    check_pthread(pthread_cond_broadcast(&runtime.state_changed));
}

static void runtime_wait(void)
{
    check_pthread(pthread_cond_wait(&runtime.state_changed, &runtime.mutex));
}

static size_t bounded_length(const char* text, size_t maximum)
{
    size_t length = 0U;
    while (length < maximum && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

static char* duplicate_text(const char* text)
{
    const size_t length = strlen(text);
    if (length == SIZE_MAX)
    {
        return NULL;
    }
    char* copy = malloc(length + 1U);
    if (copy != NULL)
    {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

/* Diagnostic allocation is best effort and never changes the operation status. */
static void set_error_locked(const char* text)
{
    char* replacement = duplicate_text(text != NULL ? text : "unknown Camera HAL runtime error");
    if (replacement != NULL)
    {
        free(runtime.last_error);
        runtime.last_error = replacement;
    }
}

static void set_error_parts_locked(const char* first, const char* second, const char* third, const char* fourth)
{
    const char* parts[] = {first, second, third, fourth};
    size_t total = 0U;
    for (size_t index = 0U; index < 4U; ++index)
    {
        const size_t length = strlen(parts[index]);
        if (length > SIZE_MAX - total - 1U)
        {
            return;
        }
        total += length;
    }
    char* replacement = malloc(total + 1U);
    if (replacement == NULL)
    {
        return;
    }
    size_t written = 0U;
    for (size_t index = 0U; index < 4U; ++index)
    {
        const size_t length = strlen(parts[index]);
        memcpy(replacement + written, parts[index], length);
        written += length;
    }
    replacement[written] = '\0';
    free(runtime.last_error);
    runtime.last_error = replacement;
}

static void clear_error_locked(void)
{
    free(runtime.last_error);
    runtime.last_error = NULL;
}

static void reset_loaded_locked(void)
{
    runtime.module_handle = NULL;
    memset(&runtime.staging, 0, sizeof(runtime.staging));
    memset(&runtime.staging_ops, 0, sizeof(runtime.staging_ops));
    memset(&runtime.active, 0, sizeof(runtime.active));
    memset(&runtime.active_ops, 0, sizeof(runtime.active_ops));
    free(runtime.backend_name);
    runtime.backend_name = NULL;
    free(runtime.backend_path);
    runtime.backend_path = NULL;
    runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    runtime.registration_attempted = false;
    runtime.client_count = 0U;
    runtime.live_instance_count = 0U;
    runtime.create_reservation_count = 0U;
}

static bool callbacks_complete(const camstream_camera_ops_v2* operations)
{
    return operations->camera_create != NULL && operations->camera_destroy != NULL && operations->camera_open != NULL &&
           operations->camera_close != NULL && operations->camera_get_capabilities != NULL &&
           operations->camera_get_stream_configuration != NULL && operations->camera_configure != NULL &&
           operations->camera_start != NULL && operations->camera_wait_frame != NULL && operations->camera_acquire_frame != NULL &&
           operations->camera_release_frame != NULL && operations->camera_stop != NULL && operations->camera_get_last_error != NULL;
}

static camstream_camera_status_t validate_binding(const camstream_camera_binding_v2* binding, const char** error)
{
    if (binding->abi_version != CAMSTREAM_CAMERA_BACKEND_ABI_VERSION_V2)
    {
        *error = "backend SPI version does not match Camera HAL binding v2";
        return CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
    }
    if (binding->struct_size != sizeof(*binding) || binding->control_funcs != &runtime.staging_ops)
    {
        *error = "backend binding size or HAL-owned operation slot is invalid";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (binding->control_funcs->struct_size != sizeof(runtime.staging_ops))
    {
        *error = "backend operation table size does not match SPI v2";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (binding->backend_name == NULL)
    {
        *error = "backend name is null";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    const size_t length = bounded_length(binding->backend_name, 128U);
    if (length == 0U || length > 127U)
    {
        *error = "backend name is empty or not bounded";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!callbacks_complete(binding->control_funcs))
    {
        *error = "backend binding has a missing mandatory callback";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static bool await_transition_locked(void)
{
    while (runtime.state == RUNTIME_LOADING || runtime.state == RUNTIME_UNLOADING)
    {
        if (pthread_equal(runtime.transition_thread, pthread_self()))
        {
            set_error_locked("recursive camera backend load/unload during module transition");
            return false;
        }
        runtime_wait();
    }
    return true;
}

static bool finish_failed_load(void* handle, const char* error)
{
    if (handle == NULL)
    {
        runtime_lock();
        reset_loaded_locked();
        set_error_locked(error);
        runtime.state = RUNTIME_UNLOADED;
        runtime_notify();
        runtime_unlock();
        return true;
    }

    runtime_lock();
    runtime.module_handle = handle;
    runtime.client_count = 0U;
    runtime.live_instance_count = 0U;
    runtime.create_reservation_count = 0U;
    runtime.state = RUNTIME_UNLOADING;
    runtime.transition_thread = pthread_self();
    set_error_locked(error);
    runtime_unlock();

    dlerror();
    const int result = dlclose(handle);
    const char* close_error = result == 0 ? NULL : dlerror();
    if (result != 0)
    {
        CAM_LOGE("failed to close rejected backend module: %s", close_error != NULL ? close_error : "unknown error");
    }
    runtime_lock();
    if (result == 0)
    {
        reset_loaded_locked();
        set_error_locked(error);
        runtime.state = RUNTIME_UNLOADED;
    }
    else
    {
        memset(&runtime.active, 0, sizeof(runtime.active));
        memset(&runtime.active_ops, 0, sizeof(runtime.active_ops));
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        runtime.registration_attempted = false;
        runtime.state = RUNTIME_FAULTED;
        set_error_parts_locked(error, "; cleanup dlclose failed: ", close_error != NULL ? close_error : "unknown error", "");
    }
    runtime_notify();
    runtime_unlock();
    return result == 0;
}

camstream_camera_status_t camstream_runtime_load_backend(const char* backend_path)
{
    if (backend_path == NULL || backend_path[0] == '\0')
    {
        camstream_runtime_set_error("camera backend path must not be empty");
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }

    /* Allocate before entering Loading so allocation failure cannot strand waiters. */
    char* path_copy = duplicate_text(backend_path);
    if (path_copy == NULL)
    {
        camstream_runtime_set_error("camera backend path allocation failed");
        return CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
    }

    runtime_lock();
    if (!await_transition_locked())
    {
        runtime_unlock();
        free(path_copy);
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (runtime.state == RUNTIME_FAULTED)
    {
        runtime_unlock();
        free(path_copy);
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    if (runtime.state == RUNTIME_LOADED)
    {
        camstream_camera_status_t status = CAMSTREAM_CAMERA_STATUS_OK;
        if (strcmp(runtime.backend_path, path_copy) != 0)
        {
            set_error_locked("a different camera backend is already active");
            status = CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
        }
        else if (runtime.client_count == UINT32_MAX)
        {
            set_error_locked("camera backend client-reference limit reached");
            status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        }
        else
        {
            ++runtime.client_count;
        }
        runtime_unlock();
        free(path_copy);
        return status;
    }

    memset(&runtime.staging, 0, sizeof(runtime.staging));
    memset(&runtime.staging_ops, 0, sizeof(runtime.staging_ops));
    memset(&runtime.active, 0, sizeof(runtime.active));
    memset(&runtime.active_ops, 0, sizeof(runtime.active_ops));
    runtime.staging.control_funcs = &runtime.staging_ops;
    free(runtime.backend_name);
    runtime.backend_name = NULL;
    runtime.backend_path = path_copy;
    runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    runtime.registration_attempted = false;
    runtime.transition_thread = pthread_self();
    set_error_locked("backend constructor did not claim the HAL binding slot");
    runtime.state = RUNTIME_LOADING;
    runtime_unlock();

    /* Constructor registration re-enters this runtime; never hold its mutex here. */
    dlerror();
    void* handle = dlopen(backend_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
    {
        const char* loader_error = dlerror();
        CAM_LOGE("dlopen failed for backend '%s': %s", backend_path, loader_error != NULL ? loader_error : "unknown error");
        finish_failed_load(NULL, loader_error != NULL ? loader_error : "dlopen failed");
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }

    runtime_lock();
    camstream_camera_status_t status = runtime.registration_status;
    if (status == CAMSTREAM_CAMERA_STATUS_OK && runtime.registration_attempted)
    {
        const char* validation_error = NULL;
        status = validate_binding(&runtime.staging, &validation_error);
        if (status == CAMSTREAM_CAMERA_STATUS_OK)
        {
            runtime.backend_name = duplicate_text(runtime.staging.backend_name);
            if (runtime.backend_name == NULL)
            {
                status = CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
                set_error_locked("backend name allocation failed");
            }
            else
            {
                runtime.active_ops = runtime.staging_ops;
                runtime.active = runtime.staging;
                runtime.active.backend_name = runtime.backend_name;
                runtime.active.control_funcs = &runtime.active_ops;
                memset(&runtime.staging, 0, sizeof(runtime.staging));
                memset(&runtime.staging_ops, 0, sizeof(runtime.staging_ops));
                runtime.module_handle = handle;
                runtime.client_count = 1U;
                runtime.live_instance_count = 0U;
                runtime.create_reservation_count = 0U;
                runtime.state = RUNTIME_LOADED;
                clear_error_locked();
                CAM_LOGI("loaded backend '%s' through SPI v2 binding", runtime.backend_name);
                runtime_notify();
                runtime_unlock();
                return CAMSTREAM_CAMERA_STATUS_OK;
            }
        }
        else
        {
            set_error_locked(validation_error);
            CAM_LOGE("invalid backend binding: %s", validation_error);
        }
    }

    char* error_copy = duplicate_text(runtime.last_error != NULL ? runtime.last_error : "backend registration failed");
    runtime_unlock();
    if (!finish_failed_load(handle, error_copy != NULL ? error_copy : "backend registration failed"))
    {
        status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    free(error_copy);
    return status;
}

camstream_camera_status_t camstream_runtime_unload_backend(void)
{
    runtime_lock();
    if (!await_transition_locked())
    {
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (runtime.state == RUNTIME_FAULTED)
    {
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    if (runtime.state != RUNTIME_LOADED || runtime.client_count == 0U)
    {
        set_error_locked("camera backend is not loaded");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (runtime.client_count > 1U)
    {
        --runtime.client_count;
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_OK;
    }
    if (runtime.live_instance_count != 0U || runtime.create_reservation_count != 0U)
    {
        set_error_locked("camera backend cannot unload while HAL instances or creates remain active");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (runtime.module_handle == NULL)
    {
        runtime.client_count = 0U;
        memset(&runtime.active, 0, sizeof(runtime.active));
        memset(&runtime.active_ops, 0, sizeof(runtime.active_ops));
        runtime.state = RUNTIME_FAULTED;
        set_error_locked("loaded camera backend has no owned module handle");
        runtime_notify();
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }

    void* handle = runtime.module_handle;
    runtime.client_count = 0U;
    runtime.state = RUNTIME_UNLOADING;
    runtime.transition_thread = pthread_self();
    runtime_unlock();

    dlerror();
    const int result = dlclose(handle);
    const char* close_error = result == 0 ? NULL : dlerror();
    if (result != 0)
    {
        CAM_LOGE("failed to unload backend module: %s", close_error != NULL ? close_error : "unknown error");
    }
    runtime_lock();
    if (result == 0)
    {
        reset_loaded_locked();
        clear_error_locked();
        runtime.state = RUNTIME_UNLOADED;
    }
    else
    {
        memset(&runtime.active, 0, sizeof(runtime.active));
        memset(&runtime.active_ops, 0, sizeof(runtime.active_ops));
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        runtime.registration_attempted = false;
        runtime.state = RUNTIME_FAULTED;
        set_error_parts_locked(
                "dlclose failed for camera backend '", runtime.backend_path, "': ", close_error != NULL ? close_error : "unknown error");
    }
    runtime_notify();
    runtime_unlock();
    return result == 0 ? CAMSTREAM_CAMERA_STATUS_OK : CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
}

camstream_camera_status_t camstream_runtime_copy_error(char* buffer, uint32_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    runtime_lock();
    const char* error = runtime.last_error != NULL ? runtime.last_error : "";
    const size_t available = (size_t)buffer_size - 1U;
    size_t length = strlen(error);
    if (length > available)
    {
        length = available;
    }
    memcpy(buffer, error, length);
    buffer[length] = '\0';
    runtime_unlock();
    return CAMSTREAM_CAMERA_STATUS_OK;
}

void camstream_runtime_set_error(const char* error)
{
    runtime_lock();
    set_error_locked(error);
    runtime_unlock();
}

camstream_camera_status_t camstream_runtime_reserve_create(const camstream_camera_binding_v2** binding)
{
    if (binding == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *binding = NULL;
    runtime_lock();
    camstream_camera_status_t status = CAMSTREAM_CAMERA_STATUS_OK;
    if (runtime.state != RUNTIME_LOADED || runtime.client_count == 0U || runtime.module_handle == NULL ||
            runtime.active.control_funcs != &runtime.active_ops)
    {
        status = CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    else if (runtime.create_reservation_count == UINT32_MAX)
    {
        status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    else
    {
        ++runtime.create_reservation_count;
        *binding = &runtime.active;
    }
    runtime_unlock();
    return status;
}

camstream_camera_status_t camstream_runtime_commit_create(void)
{
    runtime_lock();
    camstream_camera_status_t status = CAMSTREAM_CAMERA_STATUS_OK;
    if (runtime.state != RUNTIME_LOADED || runtime.client_count == 0U || runtime.module_handle == NULL ||
            runtime.active.control_funcs != &runtime.active_ops || runtime.create_reservation_count == 0U)
    {
        status = CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    else if (runtime.live_instance_count == UINT32_MAX)
    {
        status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    else
    {
        --runtime.create_reservation_count;
        ++runtime.live_instance_count;
    }
    runtime_unlock();
    return status;
}

camstream_camera_status_t camstream_runtime_cancel_create(void)
{
    runtime_lock();
    camstream_camera_status_t status = CAMSTREAM_CAMERA_STATUS_OK;
    if (runtime.state != RUNTIME_LOADED || runtime.client_count == 0U || runtime.module_handle == NULL ||
            runtime.active.control_funcs != &runtime.active_ops || runtime.create_reservation_count == 0U)
    {
        status = CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    else
    {
        --runtime.create_reservation_count;
    }
    runtime_unlock();
    return status;
}

void camstream_runtime_record_destroy(void)
{
    runtime_lock();
    if (runtime.live_instance_count > 0U)
    {
        --runtime.live_instance_count;
    }
    runtime_unlock();
}

camstream_camera_status_t camstream_camera_hal_get_binding_slot_v2(
        camstream_camera_binding_v2** binding, uint32_t binding_size, uint32_t operations_size)
{
    if (binding == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *binding = NULL;
    runtime_lock();
    if (runtime.state == RUNTIME_FAULTED)
    {
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    if (runtime.state != RUNTIME_LOADING || !pthread_equal(runtime.transition_thread, pthread_self()))
    {
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (runtime.registration_attempted)
    {
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
        set_error_locked("backend constructor claimed the binding slot more than once");
        CAM_LOGE("duplicate backend binding claim");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (binding_size != sizeof(runtime.staging) || operations_size != sizeof(runtime.staging_ops))
    {
        runtime.registration_attempted = true;
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
        set_error_locked("backend SPI v2 slot sizes do not match the HAL-owned storage");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
    }
    runtime.registration_attempted = true;
    runtime.registration_status = CAMSTREAM_CAMERA_STATUS_OK;
    *binding = &runtime.staging;
    runtime_unlock();
    return CAMSTREAM_CAMERA_STATUS_OK;
}
