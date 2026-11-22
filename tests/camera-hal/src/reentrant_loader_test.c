#define _POSIX_C_SOURCE 200809L
#include <camstream/camera/camera_hal.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static const char* active_path;
static unsigned int call_count[2];
static camstream_camera_status_t load_result[2];
static camstream_camera_status_t unload_result[2];
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static bool gate_enabled;
static bool gate_entered;
static bool gate_released;
static bool second_entered;
static bool second_finished;

static void reset_observations(void);
static int check_transition(const char* label, int phase);

void camstream_test_hold_constructor(void)
{
    pthread_mutex_lock(&gate_mutex);
    if (gate_enabled)
    {
        gate_entered = true;
        pthread_cond_broadcast(&gate_changed);
        while (!gate_released)
        {
            pthread_cond_wait(&gate_changed, &gate_mutex);
        }
    }
    pthread_mutex_unlock(&gate_mutex);
}

static void* load_on_thread(void* result)
{
    *(camstream_camera_status_t*)result = camstream_camera_hal_load_backend(active_path);
    return NULL;
}

static void* second_load_on_thread(void* result)
{
    pthread_mutex_lock(&gate_mutex);
    second_entered = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);

    *(camstream_camera_status_t*)result = camstream_camera_hal_load_backend(active_path);

    pthread_mutex_lock(&gate_mutex);
    second_finished = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);
    return NULL;
}

static int check_other_thread_wait(const char* path)
{
    active_path = path;
    reset_observations();
    pthread_mutex_lock(&gate_mutex);
    gate_enabled = true;
    gate_entered = false;
    gate_released = false;
    second_entered = false;
    second_finished = false;
    pthread_mutex_unlock(&gate_mutex);

    camstream_camera_status_t first_status = CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
    camstream_camera_status_t second_status = CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR;
    pthread_t first_thread;
    pthread_t second_thread;
    if (pthread_create(&first_thread, NULL, load_on_thread, &first_status) != 0)
    {
        return 1;
    }

    pthread_mutex_lock(&gate_mutex);
    while (!gate_entered)
    {
        pthread_cond_wait(&gate_changed, &gate_mutex);
    }
    pthread_mutex_unlock(&gate_mutex);

    if (pthread_create(&second_thread, NULL, second_load_on_thread, &second_status) != 0)
    {
        pthread_mutex_lock(&gate_mutex);
        gate_released = true;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_mutex);
        pthread_join(first_thread, NULL);
        return 1;
    }

    pthread_mutex_lock(&gate_mutex);
    while (!second_entered)
    {
        pthread_cond_wait(&gate_changed, &gate_mutex);
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!second_finished && pthread_cond_timedwait(&gate_changed, &gate_mutex, &deadline) == 0)
    {
    }
    const bool returned_early = second_finished;
    gate_released = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);

    pthread_join(first_thread, NULL);
    pthread_join(second_thread, NULL);
    gate_enabled = false;

    const bool constructor_ok = check_transition("gated constructor", 0) == 0;
    const unsigned int references =
            (first_status == CAMSTREAM_CAMERA_STATUS_OK ? 1U : 0U) + (second_status == CAMSTREAM_CAMERA_STATUS_OK ? 1U : 0U);
    bool unload_ok = true;
    for (unsigned int index = 0U; index < references; ++index)
    {
        if (camstream_camera_hal_unload_backend() != CAMSTREAM_CAMERA_STATUS_OK)
        {
            unload_ok = false;
        }
    }
    const bool destructor_ok = references != 0U && unload_ok && check_transition("gated destructor", 1) == 0;
    if (returned_early || first_status != CAMSTREAM_CAMERA_STATUS_OK || second_status != CAMSTREAM_CAMERA_STATUS_OK || !constructor_ok ||
            !unload_ok || !destructor_ok)
    {
        fprintf(stderr, "other-thread Loading wait failed: early=%d first=%d second=%d\n", returned_early, first_status, second_status);
        return 1;
    }
    return 0;
}

const char* camstream_test_reentrant_path(void)
{
    return active_path;
}

void camstream_test_record_reentrant_status(int phase, camstream_camera_status_t load_status, camstream_camera_status_t unload_status)
{
    if (phase < 0 || phase > 1)
    {
        return;
    }
    ++call_count[phase];
    load_result[phase] = load_status;
    unload_result[phase] = unload_status;
}

static int check_transition(const char* label, int phase)
{
    if (call_count[phase] != 1U || load_result[phase] != CAMSTREAM_CAMERA_STATUS_INVALID_STATE ||
            unload_result[phase] != CAMSTREAM_CAMERA_STATUS_INVALID_STATE)
    {
        fprintf(stderr, "%s: phase %d call count %u, load %d, unload %d\n", label, phase, call_count[phase], load_result[phase],
                unload_result[phase]);
        return 1;
    }
    return 0;
}

static void reset_observations(void)
{
    call_count[0] = 0U;
    call_count[1] = 0U;
    load_result[0] = CAMSTREAM_CAMERA_STATUS_OK;
    load_result[1] = CAMSTREAM_CAMERA_STATUS_OK;
    unload_result[0] = CAMSTREAM_CAMERA_STATUS_OK;
    unload_result[1] = CAMSTREAM_CAMERA_STATUS_OK;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s VALID_MODULE INVALID_MODULE\n", argv[0]);
        return 2;
    }

    active_path = argv[1];
    reset_observations();
    if (camstream_camera_hal_load_backend(active_path) != CAMSTREAM_CAMERA_STATUS_OK || check_transition("valid constructor", 0) != 0 ||
            camstream_camera_hal_unload_backend() != CAMSTREAM_CAMERA_STATUS_OK || check_transition("valid destructor", 1) != 0)
    {
        fprintf(stderr, "valid-module transition failed\n");
        return 1;
    }

    active_path = argv[2];
    reset_observations();
    if (camstream_camera_hal_load_backend(active_path) != CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED ||
            check_transition("rejected constructor", 0) != 0 || check_transition("rejected destructor", 1) != 0)
    {
        fprintf(stderr, "rejected-module cleanup transition failed\n");
        return 1;
    }

    active_path = argv[1];
    reset_observations();
    if (camstream_camera_hal_load_backend(active_path) != CAMSTREAM_CAMERA_STATUS_OK ||
            camstream_camera_hal_unload_backend() != CAMSTREAM_CAMERA_STATUS_OK)
    {
        fprintf(stderr, "runtime did not recover after rejected module\n");
        return 1;
    }

    if (check_other_thread_wait(argv[1]) != 0)
    {
        return 1;
    }

    puts("constructor/destructor reentrancy and other-thread Loading wait: PASS");
    return 0;
}
