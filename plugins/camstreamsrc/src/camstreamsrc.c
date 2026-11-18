#include <camstream/gst/camstreamsrc.h>

#define CAMSTREAM_LOG_MODULE "camstreamsrc"
#include <camstream/common/log.h>

#include <string.h>

enum
{
    WAIT_TIMEOUT_MS = 100
};

struct _CamstreamSrc
{
    GstPushSrc parent;
    GMutex lock;
    camstream_camera* camera;
    camstream_camera_stream_config_v1 active;
    GstCaps* caps;
    guint64 pending_token;
    guint64 previous_sequence;
    gboolean have_sequence;
    gboolean started;
    gboolean in_flight;
    gboolean flushing;
    gboolean stop_intent;
    gboolean faulted;
#ifdef CAMSTREAM_M4_TESTING
    gboolean fail_allocation_once;
#endif
};

G_DEFINE_TYPE(CamstreamSrc, camstream_src, GST_TYPE_PUSH_SRC)

static GstStaticPadTemplate source_template =
        GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw,format=(string)YUY2"));

static gboolean valid_active(const camstream_camera_stream_config_v1* active)
{
    return active != NULL && active->abi_version == CAMSTREAM_CAMERA_ABI_VERSION_V1 &&
           active->struct_size >= sizeof(*active) && active->pixel_format == CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV &&
           active->width >= 2U && (active->width & 1U) == 0U && active->width <= G_MAXINT && active->height > 0U &&
           active->height <= G_MAXINT && active->frame_rate_numerator > 0U &&
           active->frame_rate_numerator <= G_MAXINT && active->frame_rate_denominator > 0U &&
           active->frame_rate_denominator <= G_MAXINT && (guint64)active->width * 2U * active->height <= G_MAXSIZE;
}

gboolean camstream_src_bind(
        CamstreamSrc* source, camstream_camera* camera, const camstream_camera_stream_config_v1* active)
{
    g_return_val_if_fail(CAMSTREAM_IS_SRC(source), FALSE);
    if ((camera == NULL) != (active == NULL) || (camera != NULL && !valid_active(active)))
    {
        return FALSE;
    }
    GstCaps* caps = camera == NULL
                            ? NULL
                            : gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
                                      (gint)active->width, "height", G_TYPE_INT, (gint)active->height, "framerate",
                                      GST_TYPE_FRACTION, (gint)active->frame_rate_numerator,
                                      (gint)active->frame_rate_denominator, NULL);
    if (camera != NULL && caps == NULL)
    {
        return FALSE;
    }
    g_mutex_lock(&source->lock);
    if (source->started || source->in_flight || source->pending_token != 0U ||
            (camera != NULL && source->camera != NULL))
    {
        g_mutex_unlock(&source->lock);
        if (caps != NULL)
        {
            gst_caps_unref(caps);
        }
        return FALSE;
    }
    if (source->caps != NULL)
    {
        gst_caps_unref(source->caps);
    }
    source->caps = caps;
    source->camera = camera;
    source->active = active == NULL ? (camstream_camera_stream_config_v1){0} : *active;
    source->stop_intent = FALSE;
    source->faulted = FALSE;
    g_mutex_unlock(&source->lock);
    return TRUE;
}

void camstream_src_request_stop(CamstreamSrc* source)
{
    g_return_if_fail(CAMSTREAM_IS_SRC(source));
    g_mutex_lock(&source->lock);
    source->stop_intent = TRUE;
    source->flushing = TRUE;
    g_mutex_unlock(&source->lock);
}

gboolean camstream_src_retry_pending_release(CamstreamSrc* source)
{
    g_return_val_if_fail(CAMSTREAM_IS_SRC(source), FALSE);
    g_mutex_lock(&source->lock);
    if (source->started || source->in_flight || source->camera == NULL)
    {
        g_mutex_unlock(&source->lock);
        return FALSE;
    }
    const guint64 token = source->pending_token;
    camstream_camera* camera = source->camera;
    g_mutex_unlock(&source->lock);
    if (token == 0U)
    {
        return TRUE;
    }
    if (camstream_camera_release_frame(camera, token) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        CAM_LOGE("pending HAL frame release retry failed");
        return FALSE;
    }
    g_mutex_lock(&source->lock);
    source->pending_token = 0U;
    g_mutex_unlock(&source->lock);
    return TRUE;
}

#ifdef CAMSTREAM_M4_TESTING
void camstream_src_test_fail_allocation_once(CamstreamSrc* source)
{
    g_return_if_fail(CAMSTREAM_IS_SRC(source));
    g_mutex_lock(&source->lock);
    source->fail_allocation_once = TRUE;
    g_mutex_unlock(&source->lock);
}
#endif

static GstCaps* camstream_src_get_caps(GstBaseSrc* base, GstCaps* filter)
{
    CamstreamSrc* source = CAMSTREAM_SRC(base);
    g_mutex_lock(&source->lock);
    GstCaps* caps =
            source->caps != NULL ? gst_caps_ref(source->caps) : gst_static_pad_template_get_caps(&source_template);
    g_mutex_unlock(&source->lock);
    if (filter != NULL)
    {
        GstCaps* intersection = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(caps);
        return intersection;
    }
    return caps;
}

static gboolean camstream_src_set_caps(GstBaseSrc* base, GstCaps* caps)
{
    CamstreamSrc* source = CAMSTREAM_SRC(base);
    g_mutex_lock(&source->lock);
    const gboolean match = source->caps != NULL && gst_caps_is_equal(source->caps, caps);
    g_mutex_unlock(&source->lock);
    return match;
}

static gboolean camstream_src_start(GstBaseSrc* base)
{
    CamstreamSrc* source = CAMSTREAM_SRC(base);
    g_mutex_lock(&source->lock);
    const gboolean ready = source->camera != NULL && source->caps != NULL && source->pending_token == 0U &&
                           !source->stop_intent && !source->faulted && !source->in_flight;
    if (ready)
    {
        source->started = TRUE;
        source->flushing = FALSE;
        source->have_sequence = FALSE;
    }
    g_mutex_unlock(&source->lock);
    return ready;
}

static gboolean camstream_src_stop(GstBaseSrc* base)
{
    CamstreamSrc* source = CAMSTREAM_SRC(base);
    g_mutex_lock(&source->lock);
    source->stop_intent = TRUE;
    source->started = FALSE;
    g_mutex_unlock(&source->lock);
    return TRUE;
}

static gboolean camstream_src_unlock(GstBaseSrc* base)
{
    CamstreamSrc* source = CAMSTREAM_SRC(base);
    g_mutex_lock(&source->lock);
    source->flushing = TRUE;
    g_mutex_unlock(&source->lock);
    return TRUE;
}

static gboolean camstream_src_unlock_stop(GstBaseSrc* base)
{
    CamstreamSrc* source = CAMSTREAM_SRC(base);
    g_mutex_lock(&source->lock);
    if (!source->stop_intent)
    {
        source->flushing = FALSE;
    }
    g_mutex_unlock(&source->lock);
    return TRUE;
}

static gboolean should_flush(CamstreamSrc* source)
{
    g_mutex_lock(&source->lock);
    const gboolean result = source->flushing || source->stop_intent || !source->started;
    g_mutex_unlock(&source->lock);
    return result;
}

static gboolean valid_frame(
        const camstream_camera_frame_v1* frame, const camstream_camera_stream_config_v1* active, gsize row_bytes)
{
    if (frame->abi_version != CAMSTREAM_CAMERA_ABI_VERSION_V1 || frame->struct_size < sizeof(*frame) ||
            frame->frame_token == 0U || frame->width != active->width || frame->height != active->height ||
            frame->pixel_format != CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV || frame->plane_count != 1U)
    {
        return FALSE;
    }
    const camstream_camera_plane_v1* plane = &frame->planes[0];
    if (plane->abi_version != CAMSTREAM_CAMERA_ABI_VERSION_V1 || plane->struct_size < sizeof(*plane) ||
            plane->data == NULL || plane->stride < row_bytes)
    {
        return FALSE;
    }
    const guint64 last_row = ((guint64)frame->height - 1U) * plane->stride;
    return last_row <= G_MAXUINT64 - row_bytes && last_row + row_bytes <= G_MAXSIZE &&
           plane->bytes_used >= last_row + row_bytes && plane->allocation_size >= last_row + row_bytes &&
           plane->bytes_used <= plane->allocation_size;
}

static GstClockTime running_time(GstElement* element)
{
    GstClock* clock = gst_element_get_clock(element);
    if (clock == NULL)
    {
        return GST_CLOCK_TIME_NONE;
    }
    const GstClockTime now = gst_clock_get_time(clock);
    const GstClockTime base = gst_element_get_base_time(element);
    gst_object_unref(clock);
    return now >= base ? now - base : 0U;
}

static GstFlowReturn camstream_src_create(GstPushSrc* push, GstBuffer** published)
{
    CamstreamSrc* source = CAMSTREAM_SRC(push);
    GstBuffer* output = NULL;
    GstFlowReturn flow = GST_FLOW_ERROR;
    const char* failure = NULL;
    guint64 token = 0U;
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    camstream_camera_frame_v1 frame = {.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1, .struct_size = sizeof(frame)};
    *published = NULL;

    g_mutex_lock(&source->lock);
    if (!source->started || source->stop_intent || source->flushing)
    {
        g_mutex_unlock(&source->lock);
        return GST_FLOW_FLUSHING;
    }
    if (source->faulted || source->in_flight || source->camera == NULL)
    {
        g_mutex_unlock(&source->lock);
        return GST_FLOW_ERROR;
    }
    source->in_flight = TRUE;
    camstream_camera* camera = source->camera;
    const camstream_camera_stream_config_v1 active = source->active;
    g_mutex_unlock(&source->lock);

    for (;;)
    {
        if (should_flush(source))
        {
            flow = GST_FLOW_FLUSHING;
            goto done;
        }
        const camstream_camera_status_t waited = camstream_camera_wait_frame(camera, WAIT_TIMEOUT_MS);
        if (waited == CAMSTREAM_CAMERA_STATUS_TIMEOUT)
        {
            continue;
        }
        if (waited != CAMSTREAM_CAMERA_STATUS_OK)
        {
            failure = "HAL wait_frame failed";
            goto done;
        }
        if (should_flush(source))
        {
            flow = GST_FLOW_FLUSHING;
            goto done;
        }
        const camstream_camera_status_t acquired = camstream_camera_acquire_frame(camera, &frame);
        if (acquired == CAMSTREAM_CAMERA_STATUS_TIMEOUT)
        {
            continue;
        }
        if (acquired != CAMSTREAM_CAMERA_STATUS_OK)
        {
            failure = "HAL acquire_frame failed";
            goto done;
        }
        token = frame.frame_token;
        pts = running_time(GST_ELEMENT(source));
        break;
    }

    if (should_flush(source))
    {
        flow = GST_FLOW_FLUSHING;
        goto done;
    }
    const gsize row_bytes = (gsize)active.width * 2U;
    const gsize image_bytes = row_bytes * active.height;
    if (!valid_frame(&frame, &active, row_bytes))
    {
        failure = "HAL frame does not match active YUY2 caps or payload bounds";
        goto done;
    }

#ifdef CAMSTREAM_M4_TESTING
    g_mutex_lock(&source->lock);
    const gboolean fail_allocation = source->fail_allocation_once;
    source->fail_allocation_once = FALSE;
    g_mutex_unlock(&source->lock);
    output = fail_allocation ? NULL : gst_buffer_new_allocate(NULL, image_bytes, NULL);
#else
    output = gst_buffer_new_allocate(NULL, image_bytes, NULL);
#endif
    if (output == NULL)
    {
        failure = "GstBuffer allocation failed";
        goto done;
    }
    GstMapInfo map = GST_MAP_INFO_INIT;
    if (!gst_buffer_map(output, &map, GST_MAP_WRITE))
    {
        failure = "GstBuffer map failed";
        goto done;
    }
    if (map.size < image_bytes)
    {
        gst_buffer_unmap(output, &map);
        failure = "GstBuffer mapping is shorter than the active frame";
        goto done;
    }
    for (guint32 row = 0U; row < active.height; ++row)
    {
        memcpy(map.data + (gsize)row * row_bytes, frame.planes[0].data + (gsize)row * frame.planes[0].stride,
                row_bytes);
    }
    gst_buffer_unmap(output, &map);

    if (camstream_camera_release_frame(camera, token) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        failure = "HAL release_frame failed; token retained for session cleanup";
        g_mutex_lock(&source->lock);
        source->pending_token = token;
        source->faulted = TRUE;
        g_mutex_unlock(&source->lock);
        token = 0U;
        goto done;
    }
    token = 0U;
    GST_BUFFER_PTS(output) = pts;
    GST_BUFFER_DURATION(output) =
            gst_util_uint64_scale(GST_SECOND, active.frame_rate_denominator, active.frame_rate_numerator);
    g_mutex_lock(&source->lock);
    if (source->flushing || source->stop_intent || !source->started)
    {
        g_mutex_unlock(&source->lock);
        flow = GST_FLOW_FLUSHING;
        goto done;
    }
    if (!source->have_sequence || frame.sequence_number != source->previous_sequence + 1U)
    {
        GST_BUFFER_FLAG_SET(output, GST_BUFFER_FLAG_DISCONT);
    }
    source->previous_sequence = frame.sequence_number;
    source->have_sequence = TRUE;
    *published = output;
    output = NULL;
    g_mutex_unlock(&source->lock);
    flow = GST_FLOW_OK;

done:
    if (token != 0U && camstream_camera_release_frame(camera, token) != CAMSTREAM_CAMERA_STATUS_OK)
    {
        g_mutex_lock(&source->lock);
        source->pending_token = token;
        source->faulted = TRUE;
        g_mutex_unlock(&source->lock);
        failure = "HAL release_frame failed during cleanup; token retained";
        flow = GST_FLOW_ERROR;
    }
    if (output != NULL)
    {
        gst_buffer_unref(output);
    }
    g_mutex_lock(&source->lock);
    source->in_flight = FALSE;
    if (failure != NULL)
    {
        source->faulted = TRUE;
    }
    g_mutex_unlock(&source->lock);
    if (failure != NULL)
    {
        CAM_LOGE("%s", failure);
        GST_ELEMENT_ERROR(source, RESOURCE, FAILED, ("%s", failure), ("camstreamsrc frame transfer failed"));
    }
    return flow;
}

static void camstream_src_finalize(GObject* object)
{
    CamstreamSrc* source = CAMSTREAM_SRC(object);
    if (source->pending_token != 0U)
    {
        CAM_LOGE("source finalized while HAL frame release remains pending");
    }
    if (source->caps != NULL)
    {
        gst_caps_unref(source->caps);
    }
    g_mutex_clear(&source->lock);
    G_OBJECT_CLASS(camstream_src_parent_class)->finalize(object);
}

static void camstream_src_class_init(CamstreamSrcClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass* base_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass* push_class = GST_PUSH_SRC_CLASS(klass);
    object_class->finalize = camstream_src_finalize;
    gst_element_class_set_static_metadata(element_class, "Camera HAL source", "Source/Video",
            "Copy-first live YUY2 source using a session-owned Camera HAL", "Remote Video Monitoring");
    gst_element_class_add_static_pad_template(element_class, &source_template);
    base_class->get_caps = camstream_src_get_caps;
    base_class->set_caps = camstream_src_set_caps;
    base_class->start = camstream_src_start;
    base_class->stop = camstream_src_stop;
    base_class->unlock = camstream_src_unlock;
    base_class->unlock_stop = camstream_src_unlock_stop;
    push_class->create = camstream_src_create;
}

static void camstream_src_init(CamstreamSrc* source)
{
    g_mutex_init(&source->lock);
    gst_base_src_set_live(GST_BASE_SRC(source), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(source), GST_FORMAT_TIME);
}

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(plugin, "camstreamsrc", GST_RANK_NONE, CAMSTREAM_TYPE_SRC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, camstreamsrc, "Camera HAL copy-first source", plugin_init,
        "1.0", "unknown", "remote-video-monitoring", "local")
