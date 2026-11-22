#include <camstream/controller/gstreamer_controller.h>

#define CAMSTREAM_LOG_MODULE "controller"
#include <camstream/common/log.h>

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace camstream
{

namespace
{

constexpr guint rtp_payload_type = 26U;
constexpr guint rtp_mtu = 1200U;
constexpr guint maximum_queued_frames = 2U;
constexpr gint queue_leak_oldest = 2; // GstQueue's downstream leaky value.

bool valid_ipv4_destination(const std::string& host) noexcept
{
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) != 1)
    {
        return false;
    }
    const std::uint32_t value = ntohl(address.s_addr);
    return value >= UINT32_C(0x01000000) && value < UINT32_C(0xe0000000);
}

bool has_properties(GstElement* element, std::initializer_list<const char*> names)
{
    for (const char* name : names)
    {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), name) == nullptr)
        {
            return false;
        }
    }
    return true;
}

} // namespace

GStreamerController::GStreamerController(MediaOutputConfig media) : media_(std::move(media))
{
    if ((media_.mode != MediaOutputMode::Local && media_.mode != MediaOutputMode::RtpJpeg) ||
            (media_.mode == MediaOutputMode::Local && (!media_.host.empty() || media_.port != 0U)) ||
            (media_.mode == MediaOutputMode::RtpJpeg && (!valid_ipv4_destination(media_.host) || media_.port == 0U)))
    {
        throw std::invalid_argument("invalid media output configuration");
    }
}

GStreamerController::~GStreamerController() noexcept
{
    std::string error;
    if (!stop(error))
    {
        CAM_LOGE("unsafe pipeline teardown: %s", error.c_str());
        std::abort(); // A borrowed HAL pointer must not survive its owning service.
    }
}

void GStreamerController::on_handoff(GstElement* sink, GstBuffer* buffer, GstPad* pad, gpointer context) noexcept
{
    (void)sink;
    (void)buffer;
    (void)pad;
    auto* controller = static_cast<GStreamerController*>(context);
    controller->frames_.fetch_add(1U, std::memory_order_relaxed);
}

GstPadProbeReturn GStreamerController::on_encoded_frame(GstPad* pad, GstPadProbeInfo* info, gpointer context) noexcept
{
    (void)pad;
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0U)
    {
        auto* controller = static_cast<GStreamerController*>(context);
        controller->frames_.fetch_add(1U, std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

GstBusSyncReply GStreamerController::on_bus_message(GstBus* bus, GstMessage* message, gpointer context) noexcept
{
    (void)bus;
    auto* controller = static_cast<GStreamerController*>(context);
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR && GST_MESSAGE_TYPE(message) != GST_MESSAGE_EOS)
    {
        return GST_BUS_PASS;
    }
    GError* detail = nullptr;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
    {
        gst_message_parse_error(message, &detail, nullptr);
    }
    try
    {
        std::lock_guard<std::mutex> lock(controller->bus_error_mutex_);
        if (!controller->bus_fault_.load(std::memory_order_relaxed))
        {
            std::snprintf(controller->bus_error_, sizeof(controller->bus_error_), "%s",
                    detail == nullptr ? "GStreamer EOS or error" : detail->message);
            controller->bus_fault_.store(true, std::memory_order_release);
        }
    }
    catch (...)
    {
        controller->bus_fault_.store(true, std::memory_order_release);
    }
    g_clear_error(&detail);
    return GST_BUS_PASS;
}

bool GStreamerController::accepts(const camstream_camera_stream_config_v1& active, std::string& error) const
{
    error.clear();
    if (media_.mode == MediaOutputMode::RtpJpeg &&
            (active.pixel_format != CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV || active.width == 0U || active.width > 2040U ||
                    active.height == 0U || active.height > 2040U || (active.width % 8U) != 0U || (active.height % 8U) != 0U))
    {
        error = "RTP/JPEG requires active YUYV dimensions divisible by eight and no greater than 2040";
        return false;
    }
    return true;
}

bool GStreamerController::start(camstream_camera* camera, const camstream_camera_stream_config_v1& active,
        const std::string& plugin_path, std::string& error)
{
    error.clear();
    if (pipeline_ != nullptr || camera == nullptr || plugin_path.empty())
    {
        error = "pipeline start requires an idle controller, camera and plugin path";
        return false;
    }
    if (!accepts(active, error))
    {
        return false;
    }

    if (!gst_is_initialized())
    {
        GError* init_error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &init_error))
        {
            error = init_error == nullptr ? "GStreamer initialization failed" : init_error->message;
            g_clear_error(&init_error);
            return false;
        }
    }
    GError* plugin_error = nullptr;
    plugin_ = gst_plugin_load_file(plugin_path.c_str(), &plugin_error);
    if (plugin_ == nullptr)
    {
        error = plugin_error == nullptr ? "camstreamsrc plugin load failed" : plugin_error->message;
        g_clear_error(&plugin_error);
        return false;
    }

    pipeline_ = gst_pipeline_new("camstream-m5-session");
    source_ = CAMSTREAM_SRC(gst_element_factory_make("camstreamsrc", "camera"));
    const bool network = media_.mode == MediaOutputMode::RtpJpeg;
    sink_ = gst_element_factory_make(network ? "udpsink" : "fakesink", network ? "network-sink" : "local-sink");
    if (network)
    {
        queue_ = gst_element_factory_make("queue", "media-queue");
        encoder_ = gst_element_factory_make("jpegenc", "jpeg-encoder");
        payloader_ = gst_element_factory_make("rtpjpegpay", "jpeg-payloader");
    }
    if (pipeline_ == nullptr || source_ == nullptr || sink_ == nullptr ||
            (network && (queue_ == nullptr || encoder_ == nullptr || payloader_ == nullptr)))
    {
        error = "required GStreamer pipeline or element factory unavailable";
    }
    else if (!camstream_src_bind(source_, camera, &active))
    {
        error = "camstreamsrc rejected the active HAL session";
    }
    else
    {
        bound_ = true;
        frames_.store(0U, std::memory_order_relaxed);
#ifdef CAMSTREAM_M5_TESTING
        quiescence_started_.store(false, std::memory_order_release);
#endif
        bus_fault_.store(false, std::memory_order_relaxed);
        bus_error_[0] = '\0';
        if (!network && !has_properties(sink_, {"signal-handoffs", "sync"}))
        {
            error = "fakesink properties unavailable";
        }
        else if (network && (!has_properties(queue_, {"max-size-buffers", "max-size-bytes", "max-size-time", "leaky"}) ||
                                    !has_properties(encoder_, {"quality"}) || !has_properties(payloader_, {"pt", "mtu"}) ||
                                    !has_properties(sink_, {"host", "port", "sync", "async"})))
        {
            error = "RTP/JPEG element properties unavailable";
        }
        else
        {
            if (network)
            {
                g_object_set(queue_, "max-size-buffers", maximum_queued_frames, "max-size-bytes", 0U, "max-size-time", G_GUINT64_CONSTANT(0),
                        "leaky", queue_leak_oldest, nullptr);
                g_object_set(encoder_, "quality", 85, nullptr);
                g_object_set(payloader_, "pt", rtp_payload_type, "mtu", rtp_mtu, nullptr);
                g_object_set(sink_, "host", media_.host.c_str(), "port", static_cast<gint>(media_.port), "sync", FALSE, "async", FALSE,
                        nullptr);
            }
            else
            {
                g_object_set(sink_, "signal-handoffs", TRUE, "sync", TRUE, nullptr);
            }
        }
        if (error.empty() && !network && g_signal_connect(sink_, "handoff", G_CALLBACK(on_handoff), this) == 0U)
        {
            error = "fakesink handoff connection failed";
        }
        if (error.empty())
        {
            source_added_ = gst_bin_add(GST_BIN(pipeline_), GST_ELEMENT(source_));
            if (network)
            {
                queue_added_ = source_added_ && gst_bin_add(GST_BIN(pipeline_), queue_);
                encoder_added_ = queue_added_ && gst_bin_add(GST_BIN(pipeline_), encoder_);
                payloader_added_ = encoder_added_ && gst_bin_add(GST_BIN(pipeline_), payloader_);
                sink_added_ = payloader_added_ && gst_bin_add(GST_BIN(pipeline_), sink_);
            }
            else
            {
                sink_added_ = source_added_ && gst_bin_add(GST_BIN(pipeline_), sink_);
            }
            bool force_link_failure = false;
#ifdef CAMSTREAM_M7_TESTING
            force_link_failure = std::exchange(fail_link_for_test_, false);
#endif
            const bool linked = !force_link_failure && source_added_ && sink_added_ &&
                    (network ? gst_element_link_many(GST_ELEMENT(source_), queue_, encoder_, payloader_, sink_, nullptr)
                             : gst_element_link(GST_ELEMENT(source_), sink_));
            if (!linked)
            {
                error = "GStreamer media graph construction or link failed";
            }
            else
            {
                if (network)
                {
                    encoded_pad_ = gst_element_get_static_pad(encoder_, "src");
                    if (encoded_pad_ != nullptr)
                    {
                        encoded_probe_ = gst_pad_add_probe(encoded_pad_, GST_PAD_PROBE_TYPE_BUFFER, on_encoded_frame, this, nullptr);
                    }
                    if (encoded_probe_ == 0U)
                    {
                        error = "JPEG encoder frame observation could not be installed";
                    }
                }
                bus_ = gst_element_get_bus(pipeline_);
                if (bus_ != nullptr)
                {
                    gst_bus_set_sync_handler(bus_, on_bus_message, this, nullptr);
                }
                bool force_state_failure = false;
#ifdef CAMSTREAM_M7_TESTING
                force_state_failure = std::exchange(fail_state_for_test_, false);
#endif
                if (error.empty() && (bus_ == nullptr || force_state_failure ||
                                             gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE))
                {
                    error = "GStreamer pipeline could not enter PLAYING";
                }
                else if (error.empty())
                {
                    GstState state = GST_STATE_VOID_PENDING;
                    if (gst_element_get_state(pipeline_, &state, nullptr, 5 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
                            state != GST_STATE_PLAYING)
                    {
                        error = "GStreamer pipeline did not reach PLAYING";
                    }
                    else if (poll_bus(error))
                    {
                        return true;
                    }
                }
            }
        }
    }

    std::string cleanup_error;
    if (!stop(cleanup_error))
    {
        error += "; cleanup: " + cleanup_error;
    }
    CAM_LOGE("start failed: %s", error.c_str());
    return false;
}

bool GStreamerController::stop(std::string& error)
{
    if (pipeline_ == nullptr)
    {
        release_objects();
        return true;
    }
    std::string bus_error;
    if (!poll_bus(bus_error))
    {
        error = bus_error;
    }
    if (source_ != nullptr)
    {
        camstream_src_request_stop(source_);
    }
#ifdef CAMSTREAM_M5_TESTING
    quiescence_started_.store(true, std::memory_order_release);
#endif
    if (gst_element_set_state(pipeline_, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
    {
        error = "pipeline could not enter NULL; HAL context retained";
        return false;
    }
    GstState final_state = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(pipeline_, &final_state, nullptr, 5 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
            final_state != GST_STATE_NULL)
    {
        error = "pipeline did not quiesce; HAL context retained";
        return false;
    }
    bus_error.clear();
    if (!poll_bus(bus_error) && error.empty())
    {
        error = bus_error;
    }
    if (bound_ && (!camstream_src_retry_pending_release(source_) || !camstream_src_bind(source_, nullptr, nullptr)))
    {
        error = "pending HAL frame release or source unbind failed; HAL context retained";
        return false;
    }
    bound_ = false;
    release_objects();
    return true;
}

bool GStreamerController::poll_bus(std::string& error)
{
    if (bus_ == nullptr)
    {
        return true;
    }
    bool healthy = true;
    GstMessage* message = nullptr;
    while ((message = gst_bus_pop(bus_)) != nullptr)
    {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
        {
            GError* detail = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &detail, &debug);
            if (healthy)
            {
                error = detail == nullptr ? "GStreamer error" : detail->message;
            }
            healthy = false;
            CAM_LOGE("bus error: %s; debug=%s", detail == nullptr ? "unknown" : detail->message,
                    debug == nullptr ? "" : debug);
            g_clear_error(&detail);
            g_free(debug);
        }
        else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && healthy)
        {
            error = "GStreamer EOS";
            healthy = false;
        }
        gst_message_unref(message);
    }
    if (bus_fault_.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(bus_error_mutex_);
        if (healthy)
        {
            error = bus_error_[0] == '\0' ? "GStreamer bus fault" : bus_error_;
        }
        healthy = false;
    }
    return healthy;
}

std::uint64_t GStreamerController::frames() const noexcept
{
    return frames_.load(std::memory_order_relaxed);
}

bool GStreamerController::active() const noexcept
{
    return pipeline_ != nullptr;
}

void GStreamerController::release_objects() noexcept
{
    if (encoded_pad_ != nullptr)
    {
        if (encoded_probe_ != 0U)
        {
            gst_pad_remove_probe(encoded_pad_, encoded_probe_);
        }
        gst_object_unref(encoded_pad_);
        encoded_pad_ = nullptr;
        encoded_probe_ = 0U;
    }
    if (bus_ != nullptr)
    {
        gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
    if (pipeline_ != nullptr)
    {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    if (source_ != nullptr && !source_added_)
    {
        gst_object_unref(source_);
    }
    if (sink_ != nullptr && !sink_added_)
    {
        gst_object_unref(sink_);
    }
    if (payloader_ != nullptr && !payloader_added_)
    {
        gst_object_unref(payloader_);
    }
    if (encoder_ != nullptr && !encoder_added_)
    {
        gst_object_unref(encoder_);
    }
    if (queue_ != nullptr && !queue_added_)
    {
        gst_object_unref(queue_);
    }
    source_ = nullptr;
    sink_ = nullptr;
    queue_ = nullptr;
    encoder_ = nullptr;
    payloader_ = nullptr;
    source_added_ = false;
    sink_added_ = false;
    queue_added_ = false;
    encoder_added_ = false;
    payloader_added_ = false;
    if (plugin_ != nullptr)
    {
        gst_object_unref(plugin_);
        plugin_ = nullptr;
    }
}

} // namespace camstream
