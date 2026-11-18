#include <camstream/gst/camstreamsrc.h>

#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "camstreamsrc test failed at line %d: %s\n", __LINE__, #condition);                        \
            return FALSE;                                                                                              \
        }                                                                                                              \
    } while (0)

typedef struct observations
{
    GMutex lock;
    GCond ready;
    guint received;
    gboolean valid;
    GstClockTime previous_pts;
    GstBuffer* held;
    gboolean padded;
} observations;

static void on_handoff(GstElement* sink, GstBuffer* buffer, GstPad* pad, gpointer user_data)
{
    (void)sink;
    observations* observed = user_data;
    g_mutex_lock(&observed->lock);
    const guint sequence = observed->received;
    if (sequence < 3U)
    {
        GstCaps* caps = gst_pad_get_current_caps(pad);
        const GstStructure* fields = caps == NULL ? NULL : gst_caps_get_structure(caps, 0U);
        gint width = 0;
        gint height = 0;
        gint rate_n = 0;
        gint rate_d = 0;
        const gchar* format = fields == NULL ? NULL : gst_structure_get_string(fields, "format");
        if (fields == NULL || strcmp(gst_structure_get_name(fields), "video/x-raw") != 0 || format == NULL ||
                strcmp(format, "YUY2") != 0 || !gst_structure_get_int(fields, "width", &width) ||
                !gst_structure_get_int(fields, "height", &height) ||
                !gst_structure_get_fraction(fields, "framerate", &rate_n, &rate_d) || rate_n != 30 || rate_d != 1 ||
                width != (observed->padded ? 2 : 64) || height != (observed->padded ? 2 : 48))
        {
            observed->valid = FALSE;
        }
        if (caps != NULL)
        {
            gst_caps_unref(caps);
        }

        GstMapInfo map = GST_MAP_INFO_INIT;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        {
            observed->valid = FALSE;
        }
        else
        {
            if (map.size != (observed->padded ? 8U : 64U * 48U * 2U))
            {
                observed->valid = FALSE;
            }
            for (gsize index = 0U; index < map.size; ++index)
            {
                if (map.data[index] != (guint8)(((observed->padded ? sequence * 2U : sequence) + index) & 0xffU))
                {
                    observed->valid = FALSE;
                    break;
                }
            }
            gst_buffer_unmap(buffer, &map);
        }
        if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) ||
                (sequence > 0U && GST_BUFFER_PTS(buffer) < observed->previous_pts) ||
                GST_BUFFER_DURATION(buffer) != GST_SECOND / 30U ||
                ((sequence == 0U || observed->padded) && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)))
        {
            observed->valid = FALSE;
        }
        observed->previous_pts = GST_BUFFER_PTS(buffer);
        if (sequence == 0U)
        {
            observed->held = gst_buffer_ref(buffer);
        }
    }
    ++observed->received;
    g_cond_signal(&observed->ready);
    g_mutex_unlock(&observed->lock);
}

static gboolean wait_for_frames(observations* observed)
{
    const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_mutex_lock(&observed->lock);
    while (observed->received < 3U && g_cond_wait_until(&observed->ready, &observed->lock, deadline))
    {
    }
    const gboolean success = observed->received >= 3U && observed->valid;
    g_mutex_unlock(&observed->lock);
    return success;
}

static gboolean run_session(const char* module, const char* identifier, gboolean expect_error, gboolean fail_allocation,
        gboolean expect_timeout)
{
    observations observed = {.valid = TRUE};
    observed.padded = strcmp(identifier, "padded") == 0;
    g_mutex_init(&observed.lock);
    g_cond_init(&observed.ready);
    CHECK(camstream_camera_hal_load_backend(module) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera* camera = NULL;
    CHECK(camstream_camera_create(&camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_open(camera, identifier) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_stream_config_v1 requested = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(requested)};
    CHECK(camstream_camera_get_stream_configuration(camera, 0U, &requested) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_stream_config_v1 active = {
            .abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(active)};
    CHECK(camstream_camera_configure(camera, &requested, &active) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_start(camera) == CAMSTREAM_CAMERA_STATUS_OK);

    GstElement* pipeline = gst_pipeline_new("m4-session");
    GstElement* source = gst_element_factory_make("camstreamsrc", "camera");
    GstElement* sink = gst_element_factory_make("fakesink", "sink");
    CHECK(pipeline != NULL && source != NULL && sink != NULL);
    CHECK(camstream_src_bind(CAMSTREAM_SRC(source), camera, &active));
    g_object_set(sink, "signal-handoffs", TRUE, "sync", FALSE, NULL);
    g_signal_connect(sink, "handoff", G_CALLBACK(on_handoff), &observed);
    gst_bin_add_many(GST_BIN(pipeline), source, sink, NULL);
    CHECK(gst_element_link(source, sink));
    if (fail_allocation)
    {
        camstream_src_test_fail_allocation_once(CAMSTREAM_SRC(source));
    }
    GstBus* bus = gst_element_get_bus(pipeline);
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);

    if (expect_error)
    {
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
        CHECK(message != NULL);
        GError* error = NULL;
        gchar* debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        CHECK(error != NULL);
        g_error_free(error);
        g_free(debug);
        gst_message_unref(message);
        g_mutex_lock(&observed.lock);
        const gboolean no_frames = observed.received == 0U;
        g_mutex_unlock(&observed.lock);
        CHECK(no_frames);
    }
    else if (expect_timeout)
    {
        CHECK(gst_element_get_state(pipeline, NULL, NULL, 2 * GST_SECOND) != GST_STATE_CHANGE_FAILURE);
        g_usleep(150000U);
    }
    else
    {
        CHECK(wait_for_frames(&observed));
    }

    const gint64 stop_started = g_get_monotonic_time();
    camstream_src_request_stop(CAMSTREAM_SRC(source));
    CHECK(gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);
    CHECK(gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND) != GST_STATE_CHANGE_FAILURE);
    if (expect_error)
    {
        CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE);
        CHECK(gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);
    }
    g_mutex_lock(&observed.lock);
    const gboolean no_timeout_frames = observed.received == 0U;
    g_mutex_unlock(&observed.lock);
    CHECK(!expect_timeout || (no_timeout_frames && g_get_monotonic_time() - stop_started < G_TIME_SPAN_SECOND));
    if (strcmp(identifier, "invalid-frame") == 0)
    {
        CHECK(camstream_camera_wait_frame(camera, 1U) == CAMSTREAM_CAMERA_STATUS_INVALID_STATE);
        char diagnostic[256] = {0};
        CHECK(camstream_camera_get_last_error(camera, diagnostic, sizeof(diagnostic)) == CAMSTREAM_CAMERA_STATUS_OK);
        CHECK(strstr(diagnostic, "ownership cleanup is unresolved") != NULL);
    }
    CHECK(camstream_src_retry_pending_release(CAMSTREAM_SRC(source)));
    CHECK(camstream_src_bind(CAMSTREAM_SRC(source), NULL, NULL));
    CHECK(camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    CHECK(camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK);
    camstream_camera_destroy(camera);
    CHECK(camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK);

    if (observed.held != NULL)
    {
        GstMapInfo map = GST_MAP_INFO_INIT;
        CHECK(gst_buffer_map(observed.held, &map, GST_MAP_READ));
        CHECK(map.size == (observed.padded ? 8U : 64U * 48U * 2U) && map.data[0] == 0U && map.data[1] == 1U);
        gst_buffer_unmap(observed.held, &map);
        gst_buffer_unref(observed.held);
    }
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    g_cond_clear(&observed.ready);
    g_mutex_clear(&observed.lock);
    return TRUE;
}

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        return 2;
    }
    gst_init(&argc, &argv);
    GError* error = NULL;
    GstPlugin* plugin = gst_plugin_load_file(argv[3], &error);
    if (plugin == NULL)
    {
        fprintf(stderr, "camstreamsrc plugin load failed: %s\n", error == NULL ? "unknown error" : error->message);
        g_clear_error(&error);
        return 1;
    }
    if (!run_session(argv[1], "simulated0", FALSE, FALSE, FALSE) ||
            !run_session(argv[1], "simulated0", FALSE, FALSE, FALSE) ||
            !run_session(argv[1], "simulated0", TRUE, TRUE, FALSE) ||
            !run_session(argv[2], "acquire-fail", TRUE, FALSE, FALSE) ||
            !run_session(argv[2], "release-fail", TRUE, FALSE, FALSE) ||
            !run_session(argv[2], "invalid-frame", TRUE, FALSE, FALSE) ||
            !run_session(argv[2], "padded", FALSE, FALSE, FALSE) ||
            !run_session(argv[2], "wait-timeout", FALSE, FALSE, TRUE))
    {
        gst_object_unref(plugin);
        gst_deinit();
        return 1;
    }
    gst_object_unref(plugin);
    gst_deinit();
    puts("camstreamsrc session, copy and fault regressions: PASS");
    return 0;
}
