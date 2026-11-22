#include <camstream/controller/gstreamer_controller.h>
#include <camstream/service/camera_service.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace
{

bool expect(bool condition, const char* step)
{
    if (!condition)
    {
        std::fprintf(stderr, "M7 network test failed: %s\n", step);
    }
    return condition;
}

bool parse_number(const char* text, unsigned long maximum, unsigned long& value)
{
    if (text == nullptr || *text == '\0')
    {
        return false;
    }
    for (const char* digit = text; *digit != '\0'; ++digit)
    {
        if (*digit < '0' || *digit > '9')
        {
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    value = std::strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && value > 0UL && value <= maximum;
}

camstream_camera_stream_config_v1 request()
{
    camstream_camera_stream_config_v1 config{};
    config.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    config.struct_size = sizeof(config);
    std::memcpy(config.source_identifier, "simulated0", sizeof("simulated0"));
    config.width = 64U;
    config.height = 48U;
    config.pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    config.frame_rate_numerator = 30U;
    config.frame_rate_denominator = 1U;
    return config;
}

camstream::MediaOutputConfig network_output(std::uint16_t port)
{
    return {camstream::MediaOutputMode::RtpJpeg, "127.0.0.1", port};
}

bool wait_for_frames(camstream::CameraService& service, std::uint64_t minimum)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const camstream::CameraServiceStatus status = service.status();
        if (status.state == camstream::CameraServiceState::Faulted)
        {
            return false;
        }
        if (status.state == camstream::CameraServiceState::Streaming && status.frames >= minimum)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct Receiver
{
    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    GstElement* sink = nullptr;
    std::uint16_t port = 0U;

    ~Receiver()
    {
        close();
    }

    void close()
    {
        if (pipeline != nullptr)
        {
            (void)gst_element_set_state(pipeline, GST_STATE_NULL);
            (void)gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
        }
        if (source != nullptr)
        {
            gst_object_unref(source);
        }
        if (sink != nullptr)
        {
            gst_object_unref(sink);
        }
        if (pipeline != nullptr)
        {
            gst_object_unref(pipeline);
        }
        pipeline = nullptr;
        source = nullptr;
        sink = nullptr;
        port = 0U;
    }

    bool open(std::uint16_t requested_port = 0U)
    {
        close();
        GError* error = nullptr;
        pipeline = gst_parse_launch("udpsrc name=network-in port=0 ! rtpjitterbuffer latency=100 drop-on-latency=true ! "
                                    "rtpjpegdepay ! jpegdec ! appsink name=received sync=false max-buffers=4 drop=true",
                &error);
        if (error != nullptr)
        {
            std::fprintf(stderr, "receiver construction: %s\n", error->message);
            g_clear_error(&error);
            return false;
        }
        if (pipeline == nullptr)
        {
            return false;
        }
        source = gst_bin_get_by_name(GST_BIN(pipeline), "network-in");
        sink = gst_bin_get_by_name(GST_BIN(pipeline), "received");
        if (source == nullptr || sink == nullptr)
        {
            return false;
        }
        GstCaps* caps = gst_caps_from_string(
                "application/x-rtp,media=(string)video,encoding-name=(string)JPEG,payload=(int)26,clock-rate=(int)90000");
        if (caps == nullptr)
        {
            return false;
        }
        g_object_set(source, "port", static_cast<gint>(requested_port), "reuse", FALSE, "caps", caps, nullptr);
        gst_caps_unref(caps);
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
        {
            return false;
        }
        (void)gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
        gint allocated_port = 0;
        g_object_get(source, "port", &allocated_port, nullptr);
        if (allocated_port <= 0 || allocated_port > UINT16_MAX ||
                gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
            return false;
        }
        port = static_cast<std::uint16_t>(allocated_port);
        return true;
    }

    bool pull_frames(unsigned minimum)
    {
        GstClockTime previous = GST_CLOCK_TIME_NONE;
        for (unsigned index = 0U; index < minimum; ++index)
        {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 4 * GST_SECOND);
            if (sample == nullptr)
            {
                return false;
            }
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstCaps* caps = gst_sample_get_caps(sample);
            const GstStructure* structure = caps == nullptr || gst_caps_is_empty(caps) ? nullptr : gst_caps_get_structure(caps, 0U);
            gint width = 0;
            gint height = 0;
            const bool valid = buffer != nullptr && gst_buffer_get_size(buffer) > 0U &&
                    GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) &&
                    (!GST_CLOCK_TIME_IS_VALID(previous) || GST_BUFFER_PTS(buffer) >= previous) && structure != nullptr &&
                    gst_structure_has_name(structure, "video/x-raw") && gst_structure_get_int(structure, "width", &width) &&
                    gst_structure_get_int(structure, "height", &height) && width == 64 && height == 48;
            if (buffer != nullptr)
            {
                previous = GST_BUFFER_PTS(buffer);
            }
            gst_sample_unref(sample);
            if (!valid)
            {
                return false;
            }
        }
        return true;
    }

    bool measure(unsigned seconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        GstClockTime first_pts = GST_CLOCK_TIME_NONE;
        GstClockTime last_pts = GST_CLOCK_TIME_NONE;
        unsigned frames = 0U;
        bool valid = true;
        gint width = 0;
        gint height = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 500 * GST_MSECOND);
            if (sample == nullptr)
            {
                continue;
            }
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstCaps* caps = gst_sample_get_caps(sample);
            const GstStructure* structure = caps == nullptr || gst_caps_is_empty(caps) ? nullptr : gst_caps_get_structure(caps, 0U);
            if (buffer == nullptr || gst_buffer_get_size(buffer) == 0U || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) ||
                    structure == nullptr || !gst_structure_has_name(structure, "video/x-raw") ||
                    !gst_structure_get_int(structure, "width", &width) || !gst_structure_get_int(structure, "height", &height) ||
                    (GST_CLOCK_TIME_IS_VALID(last_pts) && GST_BUFFER_PTS(buffer) < last_pts))
            {
                valid = false;
            }
            else
            {
                if (!GST_CLOCK_TIME_IS_VALID(first_pts))
                {
                    first_pts = GST_BUFFER_PTS(buffer);
                }
                last_pts = GST_BUFFER_PTS(buffer);
                ++frames;
            }
            gst_sample_unref(sample);
        }
        std::printf("receiver_decoded_frames=%u interval_s=%u width=%d height=%d first_pts_ns=%llu last_pts_ns=%llu\n", frames,
                seconds, width, height, static_cast<unsigned long long>(first_pts), static_cast<unsigned long long>(last_pts));
        return valid && frames > 0U;
    }

};

struct HalSession
{
    camstream_camera* camera = nullptr;
    camstream_camera_stream_config_v1 active{};
    bool loaded = false;
    bool opened = false;
    bool started = false;

    bool open(const std::string& backend)
    {
        if (camstream_camera_hal_load_backend(backend.c_str()) != CAMSTREAM_CAMERA_STATUS_OK)
        {
            return false;
        }
        loaded = true;
        if (camstream_camera_create(&camera) != CAMSTREAM_CAMERA_STATUS_OK ||
                camstream_camera_open(camera, "simulated0") != CAMSTREAM_CAMERA_STATUS_OK)
        {
            return false;
        }
        opened = true;
        active.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
        active.struct_size = sizeof(active);
        const camstream_camera_stream_config_v1 requested = request();
        if (camstream_camera_configure(camera, &requested, &active) != CAMSTREAM_CAMERA_STATUS_OK ||
                camstream_camera_start(camera) != CAMSTREAM_CAMERA_STATUS_OK)
        {
            return false;
        }
        started = true;
        return true;
    }

    bool close()
    {
        bool clean = true;
        if (started)
        {
            if (camstream_camera_stop(camera) == CAMSTREAM_CAMERA_STATUS_OK)
            {
                started = false;
            }
            else
            {
                clean = false;
            }
        }
        if (!started && opened)
        {
            clean = camstream_camera_close(camera) == CAMSTREAM_CAMERA_STATUS_OK && clean;
            opened = false;
        }
        if (camera != nullptr)
        {
            camstream_camera_destroy(camera);
            camera = nullptr;
        }
        if (loaded)
        {
            clean = camstream_camera_hal_unload_backend() == CAMSTREAM_CAMERA_STATUS_OK && clean;
            loaded = false;
        }
        return clean;
    }

    ~HalSession()
    {
        (void)close();
    }
};

bool invalid_settings()
{
    auto rejected = [](camstream::MediaOutputConfig media)
    {
        try
        {
            camstream::CameraService service(std::move(media));
            (void)service;
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        return false;
    };
    return rejected({camstream::MediaOutputMode::Local, "127.0.0.1", 5004U}) &&
           rejected({camstream::MediaOutputMode::RtpJpeg, "", 5004U}) &&
           rejected({camstream::MediaOutputMode::RtpJpeg, "not-an-ip", 5004U}) &&
           rejected({camstream::MediaOutputMode::RtpJpeg, "0.0.0.1", 5004U}) &&
           rejected({camstream::MediaOutputMode::RtpJpeg, "224.0.0.1", 5004U}) &&
           rejected({camstream::MediaOutputMode::RtpJpeg, "240.0.0.1", 5004U}) &&
           rejected({camstream::MediaOutputMode::RtpJpeg, "127.0.0.1", 0U});
}

bool local_regression(const std::string& backend, const std::string& plugin)
{
    camstream::CameraService service;
    return service.configure(backend, plugin, request()).success && service.start().success && wait_for_frames(service, 3U) &&
           service.stop().success && service.status().state == camstream::CameraServiceState::Idle;
}

bool network_loopback(const std::string& backend, const std::string& plugin)
{
    Receiver receiver;
    if (!receiver.open())
    {
        return false;
    }
    const std::uint16_t port = receiver.port;
    camstream::CameraService service(network_output(port));
    for (unsigned cycle = 0U; cycle < 2U; ++cycle)
    {
        if (!service.configure(backend, plugin, request()).success || !service.start().success ||
                !wait_for_frames(service, 3U) || !receiver.pull_frames(2U) || !service.stop().success ||
                service.status().state != camstream::CameraServiceState::Idle)
        {
            return false;
        }
        if (cycle == 0U)
        {
            receiver.close(); // Discard jitterbuffer and decoder state from the previous RTP session.
            if (!receiver.open(port))
            {
                return false;
            }
        }
    }
    return true;
}

bool no_receiver(const std::string& backend, const std::string& plugin)
{
    camstream::CameraService service(network_output(50082U));
    return service.configure(backend, plugin, request()).success && service.start().success && wait_for_frames(service, 3U) &&
           service.stop().success;
}

bool fault_cleanup(const std::string& backend, const std::string& plugin)
{
    HalSession session;
    if (!session.open(backend))
    {
        return false;
    }
    camstream::GStreamerController controller(network_output(50083U));
    std::string error;
    camstream_camera_stream_config_v1 unsupported = session.active;
    unsupported.width = 66U;
    if (controller.accepts(unsupported, error) || controller.start(session.camera, unsupported, plugin, error))
    {
        return false;
    }
    controller.fail_next_link_for_test();
    if (controller.start(session.camera, session.active, plugin, error) || controller.active())
    {
        return false;
    }
    controller.fail_next_state_for_test();
    if (controller.start(session.camera, session.active, plugin, error) || controller.active())
    {
        return false;
    }
    if (!controller.start(session.camera, session.active, plugin, error))
    {
        return false;
    }
    const bool stopped = controller.stop(error) && !controller.active();
    return stopped && session.close();
}

bool missing_encoder(const std::string& backend, const std::string& plugin)
{
    GstRegistry* registry = gst_registry_get();
    GstPluginFeature* encoder = gst_registry_lookup_feature(registry, "jpegenc");
    if (encoder != nullptr)
    {
        gst_registry_remove_feature(registry, encoder);
        gst_object_unref(encoder);
    }
    camstream::CameraService service(network_output(50084U));
    return service.configure(backend, plugin, request()).success && !service.start().success &&
           service.status().state == camstream::CameraServiceState::Idle && service.stop().success;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 4 && std::strcmp(argv[1], "--receive") == 0)
    {
        unsigned long port = 0UL;
        unsigned long seconds = 0UL;
        if (!parse_number(argv[2], UINT16_MAX, port) || !parse_number(argv[3], 600UL, seconds))
        {
            return 2;
        }
        gst_init(nullptr, nullptr);
        Receiver receiver;
        return receiver.open(static_cast<std::uint16_t>(port)) && receiver.measure(static_cast<unsigned>(seconds)) ? 0 : 1;
    }
    if (argc != 3 && argc != 4)
    {
        return 2;
    }
    gst_init(nullptr, nullptr);
    const std::string backend = argv[1];
    const std::string plugin = argv[2];
    if (argc == 4)
    {
        return std::strcmp(argv[3], "--missing-encoder") == 0 && expect(missing_encoder(backend, plugin), "missing encoder cleanup") ? 0 : 1;
    }
    return expect(invalid_settings(), "media settings validation") && expect(local_regression(backend, plugin), "default local mode") &&
                   expect(network_loopback(backend, plugin), "RTP/JPEG loopback and restart") &&
                   expect(no_receiver(backend, plugin), "sender status without receiver") &&
                   expect(fault_cleanup(backend, plugin), "caps/link/state cleanup")
                   ? 0
                   : 1;
}
