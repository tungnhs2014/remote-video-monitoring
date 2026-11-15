#include "camera_backend_runtime.h"

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
    const camstream_camera_backend_v1* backend;
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
    runtime.backend = NULL;
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

static bool callbacks_complete(const camstream_camera_backend_v1* backend)
{
    return backend->create != NULL && backend->destroy != NULL && backend->open != NULL && backend->close != NULL &&
           backend->get_capabilities != NULL && backend->get_stream_configuration != NULL &&
           backend->configure != NULL && backend->start != NULL && backend->wait_frame != NULL &&
           backend->acquire_frame != NULL && backend->release_frame != NULL && backend->stop != NULL &&
           backend->get_last_error != NULL;
}

static camstream_camera_status_t validate_backend(const camstream_camera_backend_v1* backend, const char** error)
{
    if (backend == NULL)
    {
        *error = "backend registration supplied a null descriptor";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (backend->abi_version != CAMSTREAM_CAMERA_ABI_VERSION_V1)
    {
        *error = "backend ABI version does not match Camera HAL ABI v1";
        return CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED;
    }
    if (backend->struct_size < sizeof(*backend))
    {
        *error = "backend descriptor is smaller than ABI v1";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (backend->backend_name == NULL)
    {
        *error = "backend name is null";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    const size_t length = bounded_length(backend->backend_name, 128U);
    if (length == 0U || length > 127U)
    {
        *error = "backend name is empty or not bounded";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    if (!callbacks_complete(backend))
    {
        *error = "backend descriptor has a missing mandatory callback";
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    return CAMSTREAM_CAMERA_STATUS_OK;
}

static void await_transition_locked(void)
{
    while (runtime.state == RUNTIME_LOADING || runtime.state == RUNTIME_UNLOADING)
    {
        runtime_wait();
    }
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
    set_error_locked(error);
    runtime_unlock();

    dlerror();
    const int result = dlclose(handle);
    const char* close_error = result == 0 ? NULL : dlerror();
    runtime_lock();
    if (result == 0)
    {
        reset_loaded_locked();
        set_error_locked(error);
        runtime.state = RUNTIME_UNLOADED;
    }
    else
    {
        runtime.backend = NULL;
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        runtime.registration_attempted = false;
        runtime.state = RUNTIME_FAULTED;
        set_error_parts_locked(
                error, "; cleanup dlclose failed: ", close_error != NULL ? close_error : "unknown error", "");
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
    await_transition_locked();
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

    runtime.backend = NULL;
    free(runtime.backend_name);
    runtime.backend_name = NULL;
    runtime.backend_path = path_copy;
    runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    runtime.registration_attempted = false;
    set_error_locked("backend constructor did not register a descriptor");
    runtime.state = RUNTIME_LOADING;
    runtime_unlock();

    /* Constructor registration re-enters this runtime; never hold its mutex here. */
    dlerror();
    void* handle = dlopen(backend_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
    {
        const char* loader_error = dlerror();
        finish_failed_load(NULL, loader_error != NULL ? loader_error : "dlopen failed");
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }

    runtime_lock();
    camstream_camera_status_t status = runtime.registration_status;
    if (status == CAMSTREAM_CAMERA_STATUS_OK && runtime.backend != NULL)
    {
        runtime.module_handle = handle;
        runtime.client_count = 1U;
        runtime.live_instance_count = 0U;
        runtime.create_reservation_count = 0U;
        runtime.state = RUNTIME_LOADED;
        clear_error_locked();
        runtime_notify();
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_OK;
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
    await_transition_locked();
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
        runtime.backend = NULL;
        runtime.state = RUNTIME_FAULTED;
        set_error_locked("loaded camera backend has no owned module handle");
        runtime_notify();
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }

    void* handle = runtime.module_handle;
    runtime.client_count = 0U;
    runtime.state = RUNTIME_UNLOADING;
    runtime_unlock();

    dlerror();
    const int result = dlclose(handle);
    const char* close_error = result == 0 ? NULL : dlerror();
    runtime_lock();
    if (result == 0)
    {
        reset_loaded_locked();
        clear_error_locked();
        runtime.state = RUNTIME_UNLOADED;
    }
    else
    {
        runtime.backend = NULL;
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        runtime.registration_attempted = false;
        runtime.state = RUNTIME_FAULTED;
        set_error_parts_locked("dlclose failed for camera backend '", runtime.backend_path,
                "': ", close_error != NULL ? close_error : "unknown error");
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

camstream_camera_status_t camstream_runtime_reserve_create(const camstream_camera_backend_v1** backend)
{
    if (backend == NULL)
    {
        return CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT;
    }
    *backend = NULL;
    runtime_lock();
    camstream_camera_status_t status = CAMSTREAM_CAMERA_STATUS_OK;
    if (runtime.state != RUNTIME_LOADED || runtime.client_count == 0U || runtime.module_handle == NULL ||
            runtime.backend == NULL)
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
        *backend = runtime.backend;
    }
    runtime_unlock();
    return status;
}

camstream_camera_status_t camstream_runtime_commit_create(void)
{
    runtime_lock();
    camstream_camera_status_t status = CAMSTREAM_CAMERA_STATUS_OK;
    if (runtime.state != RUNTIME_LOADED || runtime.client_count == 0U || runtime.module_handle == NULL ||
            runtime.backend == NULL || runtime.create_reservation_count == 0U)
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
            runtime.backend == NULL || runtime.create_reservation_count == 0U)
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

camstream_camera_status_t camstream_camera_hal_register_backend_v1(const camstream_camera_backend_v1* backend)
{
    runtime_lock();
    if (runtime.state == RUNTIME_FAULTED)
    {
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
    }
    if (runtime.state != RUNTIME_LOADING)
    {
        set_error_locked("backend registration is valid only during HAL loading");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    if (runtime.registration_attempted)
    {
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
        set_error_locked("backend constructor registered more than once during one load");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INVALID_STATE;
    }
    runtime.registration_attempted = true;
    const char* error = NULL;
    camstream_camera_status_t status = validate_backend(backend, &error);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        runtime.registration_status = status;
        set_error_locked(error);
        runtime_unlock();
        return status;
    }
    runtime.backend_name = duplicate_text(backend->backend_name);
    if (runtime.backend_name == NULL)
    {
        runtime.registration_status = CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR;
        set_error_locked("backend name allocation failed");
        runtime_unlock();
        return CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
    }
    runtime.backend = backend;
    runtime.registration_status = CAMSTREAM_CAMERA_STATUS_OK;
    clear_error_locked();
    runtime_unlock();
    return CAMSTREAM_CAMERA_STATUS_OK;
}
