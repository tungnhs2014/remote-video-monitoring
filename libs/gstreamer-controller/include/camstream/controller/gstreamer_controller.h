#ifndef CAMSTREAM_CONTROLLER_GSTREAMER_CONTROLLER_H
#define CAMSTREAM_CONTROLLER_GSTREAMER_CONTROLLER_H

#include <camstream/camera/camera_hal.h>
#include <camstream/gst/camstreamsrc.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace camstream
{

enum class MediaOutputMode
{
    Local,
    RtpJpeg
};

/** Immutable per-process output selection. RTP/JPEG sends to one operator-configured IPv4 receiver. */
struct MediaOutputConfig
{
    MediaOutputMode mode = MediaOutputMode::Local;
    std::string host;
    std::uint16_t port = 0U;
};

/** Owns one local or RTP/JPEG pipeline and its bus. The HAL camera is borrowed. */
class GStreamerController final
{
  public:
    GStreamerController() = default;
    explicit GStreamerController(MediaOutputConfig media);
    ~GStreamerController() noexcept;
    GStreamerController(const GStreamerController&) = delete;
    GStreamerController& operator=(const GStreamerController&) = delete;

    /** Starts a pipeline for an already started HAL camera; the caller serializes lifecycle operations. */
    bool start(
            camstream_camera* camera, const camstream_camera_stream_config_v1& active, const std::string& plugin_path, std::string& error);

    /** Checks the driver-confirmed mode against the selected output before a service session is committed. */
    bool accepts(const camstream_camera_stream_config_v1& active, std::string& error) const;

    /** Quiesces callbacks, resolves pending HAL release, then drops the borrowed camera and owned Gst objects. */
    bool stop(std::string& error);

    /** Drains this controller's bus; returns false on ERROR or EOS. No other consumer may drain this bus. */
    bool poll_bus(std::string& error);
    std::uint64_t frames() const noexcept;
    bool active() const noexcept;
#ifdef CAMSTREAM_M7_TESTING
    /** Test-only failure injection after element creation. */
    void fail_next_link_for_test() noexcept
    {
        fail_link_for_test_ = true;
    }
    void fail_next_state_for_test() noexcept
    {
        fail_state_for_test_ = true;
    }
#endif
#ifdef CAMSTREAM_M5_TESTING
    /** Test-only observation of Stop entering pipeline quiescence. */
    bool quiescence_started_for_test() const noexcept
    {
        return quiescence_started_.load(std::memory_order_acquire);
    }
#endif

  private:
    static void on_handoff(GstElement* sink, GstBuffer* buffer, GstPad* pad, gpointer context) noexcept;
    static GstPadProbeReturn on_encoded_frame(GstPad* pad, GstPadProbeInfo* info, gpointer context) noexcept;
    static GstBusSyncReply on_bus_message(GstBus* bus, GstMessage* message, gpointer context) noexcept;
    void release_objects() noexcept;

    const MediaOutputConfig media_{};
    GstPlugin* plugin_ = nullptr;
    GstElement* pipeline_ = nullptr;
    CamstreamSrc* source_ = nullptr; // Borrowed from pipeline after gst_bin_add.
    GstElement* sink_ = nullptr;     // Borrowed from pipeline after gst_bin_add.
    GstElement* queue_ = nullptr;
    GstElement* encoder_ = nullptr;
    GstElement* payloader_ = nullptr;
    GstPad* encoded_pad_ = nullptr; // Owned pad reference until pipeline release.
    gulong encoded_probe_ = 0U;
    GstBus* bus_ = nullptr;          // Owned reference from gst_element_get_bus.
    bool source_added_ = false;
    bool sink_added_ = false;
    bool queue_added_ = false;
    bool encoder_added_ = false;
    bool payloader_added_ = false;
    bool bound_ = false;
#ifdef CAMSTREAM_M7_TESTING
    bool fail_link_for_test_ = false;
    bool fail_state_for_test_ = false;
#endif
    std::atomic<std::uint64_t> frames_{0};
#ifdef CAMSTREAM_M5_TESTING
    std::atomic<bool> quiescence_started_{false};
#endif
    std::mutex bus_error_mutex_;
    std::atomic<bool> bus_fault_{false};
    char bus_error_[256] = {0};
};

} // namespace camstream

#endif
