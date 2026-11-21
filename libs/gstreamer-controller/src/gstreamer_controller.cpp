#include <camstream/controller/gstreamer_controller.h>

#define CAMSTREAM_LOG_MODULE "controller"
#include <camstream/common/log.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace camstream
{

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

bool GStreamerController::start(camstream_camera* camera, const camstream_camera_stream_config_v1& active,
        const std::string& plugin_path, std::string& error)
{
    if (pipeline_ != nullptr || camera == nullptr || plugin_path.empty())
    {
        error = "pipeline start requires an idle controller, camera and plugin path";
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
    sink_ = gst_element_factory_make("fakesink", "local-sink");
    if (pipeline_ == nullptr || source_ == nullptr || sink_ == nullptr)
    {
        error = "GStreamer pipeline, camstreamsrc or fakesink factory unavailable";
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
        g_object_set(sink_, "signal-handoffs", TRUE, "sync", TRUE, nullptr);
        if (g_signal_connect(sink_, "handoff", G_CALLBACK(on_handoff), this) == 0U)
        {
            error = "fakesink handoff connection failed";
        }
        else
        {
            source_added_ = gst_bin_add(GST_BIN(pipeline_), GST_ELEMENT(source_));
            sink_added_ = gst_bin_add(GST_BIN(pipeline_), sink_);
            if (!source_added_ || !sink_added_ || !gst_element_link(GST_ELEMENT(source_), sink_))
            {
                error = "GStreamer source/sink construction or link failed";
            }
            else
            {
                bus_ = gst_element_get_bus(pipeline_);
                if (bus_ != nullptr)
                {
                    gst_bus_set_sync_handler(bus_, on_bus_message, this, nullptr);
                }
                if (bus_ == nullptr || gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
                {
                    error = "GStreamer pipeline could not enter PLAYING";
                }
                else
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
    source_ = nullptr;
    sink_ = nullptr;
    source_added_ = false;
    sink_added_ = false;
    if (plugin_ != nullptr)
    {
        gst_object_unref(plugin_);
        plugin_ = nullptr;
    }
}

} // namespace camstream
