#ifndef CAMSTREAM_V4L2_INTERNAL_H
#define CAMSTREAM_V4L2_INTERNAL_H

#include <camstream/camera/camera_backend.h>

#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct v4l2_stream_configuration
{
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t frame_rate_numerator;
    uint32_t frame_rate_denominator;
} v4l2_stream_configuration;

typedef struct configuration_list
{
    v4l2_stream_configuration* items;
    size_t count;
    size_t capacity;
} configuration_list;

typedef struct mapped_buffer
{
    void* address;
    size_t length;
    uint64_t frame_token;
    bool queued;
    bool outstanding;
} mapped_buffer;

enum
{
    V4L2_REQUESTED_BUFFER_COUNT = 4U
};

struct camstream_camera_instance
{
    int fd;
    char source_identifier[CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE];
    configuration_list configurations;
    char* last_error;
    camstream_camera_stream_config_v1 active_configuration;
    uint32_t allocated_buffer_count;
    bool buffer_release_required;
    mapped_buffer* mapped_buffers;
    uint32_t mapped_buffer_count;
    uint32_t active_stride;
    uint64_t next_frame_token;
    bool configured;
    bool reopen_required;
    bool streaming;
    bool streamoff_required;
};

int v4l2_open_device(const char* path);
int v4l2_ioctl_device(int fd, unsigned long request, void* argument);
int v4l2_close_device(int fd);
int v4l2_poll_device(struct pollfd* descriptor, int timeout_ms);
void* v4l2_mmap_device(size_t length, int fd, off_t offset);
int v4l2_munmap_device(void* address, size_t length);

size_t v4l2_bounded_length(const char* text, size_t maximum);
bool v4l2_valid_header(uint32_t abi_version, uint32_t struct_size, size_t minimum_size);
camstream_camera_status_t v4l2_set_error(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* message);
camstream_camera_status_t v4l2_set_errno_status(
        camstream_camera_instance* camera, camstream_camera_status_t status, const char* operation, int error_number);
camstream_camera_status_t v4l2_set_errno_error(
        camstream_camera_instance* camera, const char* operation, int error_number);
void v4l2_append_errno_detail(
        camstream_camera_instance* camera, const char* phase, const char* operation, int error_number);
void v4l2_clear_error(camstream_camera_instance* camera);

int v4l2_release_requested_buffers(int fd);
int v4l2_streamoff_device(int fd);
bool v4l2_unmap_buffers(camstream_camera_instance* camera);
camstream_camera_status_t v4l2_enumerate_configurations(
        camstream_camera_instance* camera, int fd, configuration_list* list);
camstream_camera_status_t v4l2_fail_configuration(camstream_camera_instance* camera, camstream_camera_status_t failure);
camstream_camera_status_t v4l2_allocate_buffers(camstream_camera_instance* camera, uint32_t image_size);

camstream_camera_status_t v4l2_create(camstream_camera_instance** instance);
void v4l2_destroy(camstream_camera_instance* camera);
camstream_camera_status_t v4l2_open(camstream_camera_instance* camera, const char* source_identifier);
camstream_camera_status_t v4l2_close(camstream_camera_instance* camera);
camstream_camera_status_t v4l2_get_capabilities(
        camstream_camera_instance* camera, camstream_camera_capabilities_v1* capabilities);
camstream_camera_status_t v4l2_get_stream_configuration(
        camstream_camera_instance* camera, uint32_t index, camstream_camera_stream_config_v1* configuration);
camstream_camera_status_t v4l2_configure(camstream_camera_instance* camera,
        const camstream_camera_stream_config_v1* requested, camstream_camera_stream_config_v1* active);
camstream_camera_status_t v4l2_start(camstream_camera_instance* camera);
camstream_camera_status_t v4l2_wait_frame(camstream_camera_instance* camera, uint32_t timeout_ms);
camstream_camera_status_t v4l2_acquire_frame(camstream_camera_instance* camera, camstream_camera_frame_v1* frame);
camstream_camera_status_t v4l2_release_frame(camstream_camera_instance* camera, uint64_t frame_token);
camstream_camera_status_t v4l2_stop(camstream_camera_instance* camera);
camstream_camera_status_t v4l2_get_last_error(camstream_camera_instance* camera, char* buffer, uint32_t buffer_size);

#endif
