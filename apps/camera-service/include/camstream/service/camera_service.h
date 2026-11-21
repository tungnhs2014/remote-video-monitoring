#ifndef CAMSTREAM_SERVICE_CAMERA_SERVICE_H
#define CAMSTREAM_SERVICE_CAMERA_SERVICE_H

#include <camstream/camera/camera_hal.h>
#include <camstream/controller/gstreamer_controller.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace camstream
{

enum class CameraServiceState
{
    Idle,
    Configured,
    Streaming,
    Faulted
};

struct CameraServiceResult
{
    bool success;
    camstream_camera_status_t hal_status;
    std::string detail;
};

struct CameraServiceStatus
{
    CameraServiceState state;
    camstream_camera_stream_config_v1 active;
    std::uint64_t frames;
    std::string detail;
};

/**
 * Owns one HAL camera and backend module client reference per configured session.
 * Configure -> Start -> Stop releases the context; Restart requires Configure again.
 * Public calls are serialized internally. The source borrows HAL only while the controller is active.
 */
class CameraService final
{
  public:
    CameraService() = default;
    ~CameraService() noexcept;
    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    /** Loads one backend, opens the requested source and returns its driver-confirmed active mode. */
    CameraServiceResult configure(const std::string& backend_path, const std::string& plugin_path,
            const camstream_camera_stream_config_v1& requested);

    /** Starts HAL before the source pipeline. Valid only after successful Configure. */
    CameraServiceResult start();

    /** Quiesces the pipeline before HAL cleanup and final module unload. Safe to repeat from Idle. */
    CameraServiceResult stop();

    /** Returns current state and drains this service's bus errors without touching HAL during streaming. */
    CameraServiceStatus status();
#ifdef CAMSTREAM_M5_TESTING
    /** Test-only observation; does not enter the serialized control-call mutex. */
    bool quiescence_started_for_test() const noexcept
    {
        return controller_.quiescence_started_for_test();
    }
#endif

  private:
    CameraServiceResult configure_locked(const std::string& backend_path, const std::string& plugin_path,
            const camstream_camera_stream_config_v1& requested);
    CameraServiceResult start_locked();
    CameraServiceResult stop_locked();
    CameraServiceResult hal_failure(const char* operation, camstream_camera_status_t code) const;

    std::mutex operation_mutex_;
    GStreamerController controller_;
    camstream_camera* camera_ = nullptr;
    bool module_loaded_ = false;
    bool opened_ = false;
    bool started_ = false;
    CameraServiceState state_ = CameraServiceState::Idle;
    camstream_camera_stream_config_v1 requested_{};
    camstream_camera_stream_config_v1 active_{};
    std::string backend_path_;
    std::string plugin_path_;
    std::string last_error_;
};

} // namespace camstream

#endif
