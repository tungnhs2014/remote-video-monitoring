#include <camstream/gst/camstreamsrc.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct smoke_observation
{
    GMutex mutex;
    CamstreamSrc* source;
    uint32_t cycle;
    uint32_t count;
    uint32_t required;
    uint32_t width;
    uint32_t height;
    uint32_t rate_numerator;
    uint32_t rate_denominator;
    GstClockTime duration;
    GstClockTime previous_pts;
    bool valid;
} smoke_observation;

static bool parse_positive(const char* text, uint32_t maximum, uint32_t* value)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-')
    {
        return false;
    }
    errno = 0;
    char* end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0UL || parsed > maximum)
    {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static void report_hal_error(camstream_camera* camera, const char* operation, camstream_camera_status_t status)
{
    char detail[256] = {0};
    if (camera != NULL)
    {
        (void)camstream_camera_get_last_error(camera, detail, sizeof(detail));
    }
    else
    {
        (void)camstream_camera_hal_get_last_error(detail, sizeof(detail));
    }
    fprintf(stderr, "HAL %s failed: status=%" PRId32 " detail=%s\n", operation, status, detail);
}

static void on_frame(GstElement* sink, GstBuffer* buffer, GstPad* pad, gpointer user_data)
{
    (void)sink;
    smoke_observation* observed = user_data;
    g_mutex_lock(&observed->mutex);

    if (observed->count == 0U)
    {
        GstCaps* caps = gst_pad_get_current_caps(pad);
        const GstStructure* fields = caps == NULL ? NULL : gst_caps_get_structure(caps, 0U);
        const gchar* format = fields == NULL ? NULL : gst_structure_get_string(fields, "format");
        gint width = 0;
        gint height = 0;
        gint rate_numerator = 0;
        gint rate_denominator = 0;
        if (fields == NULL || strcmp(gst_structure_get_name(fields), "video/x-raw") != 0 || format == NULL ||
                strcmp(format, "YUY2") != 0 || !gst_structure_get_int(fields, "width", &width) ||
                !gst_structure_get_int(fields, "height", &height) ||
                !gst_structure_get_fraction(fields, "framerate", &rate_numerator, &rate_denominator) ||
                width != (gint)observed->width || height != (gint)observed->height || rate_numerator <= 0 ||
                rate_denominator <= 0 ||
                (uint64_t)rate_numerator * observed->rate_denominator !=
                        (uint64_t)observed->rate_numerator * rate_denominator)
        {
            observed->valid = false;
        }
        if (caps != NULL)
        {
            gchar* description = gst_caps_to_string(caps);
            printf("CAPS cycle=%" PRIu32 " %s\n", observed->cycle, description == NULL ? "unavailable" : description);
            g_free(description);
            gst_caps_unref(caps);
        }
    }

    const gsize expected_size = (gsize)observed->width * 2U * observed->height;
    GstMapInfo map = GST_MAP_INFO_INIT;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        observed->valid = false;
    }
    else
    {
        if (map.size != expected_size)
        {
            observed->valid = false;
        }
        gst_buffer_unmap(buffer, &map);
    }
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    const GstClockTime duration = GST_BUFFER_DURATION(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts) || duration != observed->duration ||
            (observed->count > 0U && pts < observed->previous_pts) ||
            (observed->count == 0U && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)))
    {
        observed->valid = false;
    }
    observed->previous_pts = pts;
    ++observed->count;
    printf("FRAME cycle=%" PRIu32 " count=%" PRIu32 " bytes=%zu pts_ns=%" G_GUINT64_FORMAT
           " duration_ns=%" G_GUINT64_FORMAT " discont=%u\n",
            observed->cycle, observed->count, gst_buffer_get_size(buffer), pts, duration,
            GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) ? 1U : 0U);
    fflush(stdout);
    if (observed->count >= observed->required || !observed->valid)
    {
        camstream_src_request_stop(observed->source);
    }
    g_mutex_unlock(&observed->mutex);
}

static bool wait_for_frames(GstBus* bus, smoke_observation* observed, uint32_t required)
{
    const gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline)
    {
        g_mutex_lock(&observed->mutex);
        const bool enough = observed->count >= required;
        const bool valid = observed->valid;
        g_mutex_unlock(&observed->mutex);
        if (!valid || enough)
        {
            return valid && enough;
        }
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (message != NULL)
        {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            {
                GError* error = NULL;
                gchar* debug = NULL;
                gst_message_parse_error(message, &error, &debug);
                fprintf(stderr, "GStreamer error: %s; debug=%s\n", error == NULL ? "unknown" : error->message,
                        debug == NULL ? "" : debug);
                g_clear_error(&error);
                g_free(debug);
            }
            else
            {
                fprintf(stderr, "GStreamer EOS before requested frame count\n");
            }
            gst_message_unref(message);
            return false;
        }
    }
    fprintf(stderr, "Timed out waiting for %" PRIu32 " frames\n", required);
    return false;
}

static bool run_cycle(const char* identifier, const camstream_camera_stream_config_v1* requested,
        uint32_t required_frames, uint32_t cycle)
{
    camstream_camera* camera = NULL;
    GstElement* pipeline = NULL;
    GstElement* source = NULL;
    GstElement* sink = NULL;
    GstBus* bus = NULL;
    bool opened = false;
    bool started = false;
    bool source_added = false;
    bool sink_added = false;
    bool bound = false;
    bool passed = false;
    camstream_camera_status_t status = camstream_camera_create(&camera);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        report_hal_error(NULL, "create", status);
        return false;
    }

    status = camstream_camera_open(camera, identifier);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        report_hal_error(camera, "open", status);
        goto cleanup;
    }
    opened = true;
    camstream_camera_stream_config_v1 active = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(active)};
    status = camstream_camera_configure(camera, requested, &active);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        report_hal_error(camera, "configure", status);
        goto cleanup;
    }
    printf("ACTIVE cycle=%" PRIu32 " source=%s width=%" PRIu32 " height=%" PRIu32 " format=YUYV fps=%" PRIu32
           "/%" PRIu32 "\n",
            cycle, active.source_identifier, active.width, active.height, active.frame_rate_numerator,
            active.frame_rate_denominator);
    if (active.width != requested->width || active.height != requested->height ||
            active.pixel_format != CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV || active.frame_rate_numerator == 0U ||
            active.frame_rate_denominator == 0U ||
            (uint64_t)active.frame_rate_numerator * requested->frame_rate_denominator !=
                    (uint64_t)requested->frame_rate_numerator * active.frame_rate_denominator)
    {
        fprintf(stderr, "Driver-confirmed active configuration differs from requested smoke mode\n");
        goto cleanup;
    }
    status = camstream_camera_start(camera);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        report_hal_error(camera, "start", status);
        goto cleanup;
    }
    started = true;

    pipeline = gst_pipeline_new("camstream-m4-smoke");
    source = gst_element_factory_make("camstreamsrc", "camera");
    sink = gst_element_factory_make("fakesink", "sink");
    if (pipeline == NULL || source == NULL || sink == NULL)
    {
        fprintf(stderr, "GStreamer pipeline, camstreamsrc or fakesink factory unavailable\n");
        goto cleanup;
    }
    if (!camstream_src_bind(CAMSTREAM_SRC(source), camera, &active))
    {
        fprintf(stderr, "camstreamsrc rejected the active session\n");
        goto cleanup;
    }
    bound = true;
    smoke_observation observed = {.source = CAMSTREAM_SRC(source),
            .cycle = cycle,
            .required = required_frames,
            .width = active.width,
            .height = active.height,
            .rate_numerator = active.frame_rate_numerator,
            .rate_denominator = active.frame_rate_denominator,
            .duration = gst_util_uint64_scale(GST_SECOND, active.frame_rate_denominator, active.frame_rate_numerator),
            .valid = true};
    g_mutex_init(&observed.mutex);
    g_object_set(sink, "signal-handoffs", TRUE, "sync", TRUE, NULL);
    g_signal_connect(sink, "handoff", G_CALLBACK(on_frame), &observed);
    source_added = gst_bin_add(GST_BIN(pipeline), source);
    sink_added = gst_bin_add(GST_BIN(pipeline), sink);
    if (!source_added || !sink_added)
    {
        fprintf(stderr, "GStreamer pipeline could not own source or sink\n");
        goto stop_pipeline;
    }
    if (!gst_element_link(source, sink))
    {
        fprintf(stderr, "camstreamsrc could not link to fakesink\n");
        goto stop_pipeline;
    }
    bus = gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        fprintf(stderr, "GStreamer pipeline could not enter PLAYING\n");
        goto stop_pipeline;
    }
    passed = wait_for_frames(bus, &observed, required_frames);

stop_pipeline:
    camstream_src_request_stop(CAMSTREAM_SRC(source));
    if (gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
    {
        fprintf(stderr, "Pipeline could not enter NULL; terminating with borrowed HAL context intact\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    GstState final_state = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(pipeline, &final_state, NULL, 5 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
            final_state != GST_STATE_NULL)
    {
        fprintf(stderr, "Pipeline did not quiesce; terminating with borrowed HAL context intact\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    if (!camstream_src_retry_pending_release(CAMSTREAM_SRC(source)) ||
            !camstream_src_bind(CAMSTREAM_SRC(source), NULL, NULL))
    {
        fprintf(stderr, "HAL frame cleanup unresolved; context retained until process exit\n");
        return false;
    }
    bound = false;
    g_mutex_lock(&observed.mutex);
    const uint32_t observed_count = observed.count;
    const bool metadata_valid = observed.valid;
    g_mutex_unlock(&observed.mutex);
    printf("SUMMARY cycle=%" PRIu32 " frames=%" PRIu32 " requested=%" PRIu32 " metadata=%s\n", cycle, observed_count,
            required_frames, metadata_valid ? "PASS" : "FAIL");
    passed = passed && metadata_valid && observed_count >= required_frames;
    g_mutex_clear(&observed.mutex);

cleanup:
    if (bound && source != NULL)
    {
        camstream_src_request_stop(CAMSTREAM_SRC(source));
        (void)camstream_src_bind(CAMSTREAM_SRC(source), NULL, NULL);
    }
    if (bus != NULL)
    {
        gst_object_unref(bus);
    }
    if (pipeline != NULL)
    {
        gst_object_unref(pipeline);
    }
    if (!source_added && source != NULL)
    {
        gst_object_unref(source);
    }
    if (!sink_added && sink != NULL)
    {
        gst_object_unref(sink);
    }
    if (started)
    {
        status = camstream_camera_stop(camera);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            report_hal_error(camera, "stop", status);
            passed = false;
            opened = false;
        }
    }
    if (opened)
    {
        status = camstream_camera_close(camera);
        if (status != CAMSTREAM_CAMERA_STATUS_OK)
        {
            report_hal_error(camera, "close", status);
            passed = false;
        }
    }
    camstream_camera_destroy(camera);
    return passed;
}

int main(int argc, char* argv[])
{
    if (argc != 10)
    {
        fprintf(stderr, "usage: %s BACKEND_SO PLUGIN_SO SOURCE WIDTH HEIGHT FPS_NUM FPS_DEN FRAMES CYCLES\n", argv[0]);
        return 2;
    }
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t rate_numerator = 0U;
    uint32_t rate_denominator = 0U;
    uint32_t frames = 0U;
    uint32_t cycles = 0U;
    if (strlen(argv[3]) >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE || !parse_positive(argv[4], G_MAXINT, &width) ||
            !parse_positive(argv[5], G_MAXINT, &height) || !parse_positive(argv[6], G_MAXINT, &rate_numerator) ||
            !parse_positive(argv[7], G_MAXINT, &rate_denominator) || !parse_positive(argv[8], 300U, &frames) ||
            !parse_positive(argv[9], 5U, &cycles) || (width & 1U) != 0U)
    {
        fprintf(stderr, "invalid source, YUYV dimensions, frame rate, frame count or cycle count\n");
        return 2;
    }
    camstream_camera_stream_config_v1 requested = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1,
            .struct_size = sizeof(requested),
            .width = width,
            .height = height,
            .pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV,
            .frame_rate_numerator = rate_numerator,
            .frame_rate_denominator = rate_denominator};
    memcpy(requested.source_identifier, argv[3], strlen(argv[3]) + 1U);

    gst_init(&argc, &argv);
    GError* plugin_error = NULL;
    GstPlugin* plugin = gst_plugin_load_file(argv[2], &plugin_error);
    if (plugin == NULL)
    {
        fprintf(stderr, "plugin load failed: %s\n", plugin_error == NULL ? "unknown" : plugin_error->message);
        g_clear_error(&plugin_error);
        return 1;
    }
    camstream_camera_status_t status = camstream_camera_hal_load_backend(argv[1]);
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        report_hal_error(NULL, "load_backend", status);
        gst_object_unref(plugin);
        return 1;
    }
    bool passed = true;
    for (uint32_t cycle = 1U; cycle <= cycles; ++cycle)
    {
        if (!run_cycle(argv[3], &requested, frames, cycle))
        {
            passed = false;
            break;
        }
    }
    if (!passed)
    {
        fprintf(stderr, "Session smoke failed; any unresolved context remains held until process exit\n");
        return 1;
    }
    status = camstream_camera_hal_unload_backend();
    if (status != CAMSTREAM_CAMERA_STATUS_OK)
    {
        report_hal_error(NULL, "unload_backend", status);
        passed = false;
    }
    gst_object_unref(plugin);
    gst_deinit();
    printf("RESULT %s cycles=%" PRIu32 "\n", passed ? "PASS" : "FAIL", cycles);
    return passed ? 0 : 1;
}
