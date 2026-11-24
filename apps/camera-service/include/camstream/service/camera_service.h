#ifndef CAMSTREAM_SERVICE_CAMERA_SERVICE_H
#define CAMSTREAM_SERVICE_CAMERA_SERVICE_H

#include <camstream/camera/camera_hal.h>
#include <camstream/controller/gstreamer_controller.h>

#include <cstdint>
#ifdef CAMSTREAM_M6_TESTING
#include <functional>
#endif
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace camstream
{

enum class CameraServiceState
{
    Idle,
    Configured,
    Streaming,
    Faulted
};

enum class CameraServiceOperation
{
    None,
    Configure,
    Start,
    Stop,
    Discover
};

/** Snapshot of the authoritative session and most recent operation.
 * generation advances on state changes within this process; it is not a persistent session ID.
 * frames counts the controller's local observation point, not remote video delivery.
 */
struct CameraServiceStatus
{
    CameraServiceState state;
    camstream_camera_stream_config_v1 requested;
    camstream_camera_stream_config_v1 active;
    std::uint64_t frames;
    std::uint64_t generation;
    CameraServiceOperation last_operation;
    bool last_operation_success;
    camstream_camera_status_t last_operation_status;
    std::string detail;
};

/** Operation outcome; committed_status is present only after success. */
struct CameraServiceResult
{
    bool success;
    camstream_camera_status_t hal_status;
    std::string detail;
    std::optional<CameraServiceStatus> committed_status = std::nullopt;
};

/** Enumerated HAL modes and their operation result. */
struct CameraServiceCapabilities
{
    CameraServiceResult result;
    std::uint32_t maximum_plane_count;
    std::vector<camstream_camera_stream_config_v1> modes;
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
    /** Fixes the operator-selected media output for this service lifetime; no RPC can change it. */
    explicit CameraService(MediaOutputConfig media);
    ~CameraService() noexcept;
    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    /** Loads one backend, opens the requested source and reports its backend-confirmed active mode.
     * Requires Idle, or an identical request while Configured. A supplied expected_generation must match
     * the current snapshot. If a post-load step fails, cleanup is attempted before Configured is committed.
     */
    CameraServiceResult configure(const std::string& backend_path, const std::string& plugin_path,
            const camstream_camera_stream_config_v1& requested, std::optional<std::uint64_t> expected_generation = std::nullopt);

    /** Starts HAL before the source pipeline; requires Configured and checks expected_generation when supplied. */
    CameraServiceResult start(std::optional<std::uint64_t> expected_generation = std::nullopt);

    /** Quiesces the pipeline before HAL cleanup and final module unload. Safe to repeat from Idle.
     * A failed quiescence retains the borrowed camera; expected_generation is checked when supplied.
     */
    CameraServiceResult stop(std::optional<std::uint64_t> expected_generation = std::nullopt);

    /** Enumerates HAL-reported modes while Idle using a temporary service-owned camera, then releases it.
     * This does not prove physical device availability after the call returns.
     */
    CameraServiceCapabilities discover(const std::string& backend_path, const std::string& source_identifier);

    /** Returns current state and drains bus errors without touching HAL during streaming.
     * This call is serialized with control operations and may transition Streaming to Faulted.
     */
    CameraServiceStatus status();
#ifdef CAMSTREAM_M6_TESTING
    void set_before_configure_hook(std::function<void()> hook);
#endif
#ifdef CAMSTREAM_M5_TESTING
    /** Test-only observation; does not enter the serialized control-call mutex. */
    bool quiescence_started_for_test() const noexcept
    {
        return controller_.quiescence_started_for_test();
    }
#endif

  private:
    CameraServiceResult configure_locked(
            const std::string& backend_path, const std::string& plugin_path, const camstream_camera_stream_config_v1& requested);
    CameraServiceResult start_locked();
    CameraServiceResult stop_locked();
    CameraServiceResult hal_failure(const char* operation, camstream_camera_status_t code) const;
    CameraServiceResult finish_locked(CameraServiceOperation operation, CameraServiceState previous, CameraServiceResult result);
    CameraServiceResult check_generation_locked(std::optional<std::uint64_t> expected_generation) const;

    std::mutex operation_mutex_;
    GStreamerController controller_;
    camstream_camera* camera_ = nullptr;
    bool module_loaded_ = false;
    bool opened_ = false;
    bool started_ = false;
    CameraServiceState state_ = CameraServiceState::Idle;
    std::uint64_t generation_ = 0U;
    CameraServiceOperation last_operation_ = CameraServiceOperation::None;
    bool last_operation_success_ = true;
    camstream_camera_status_t last_operation_status_ = CAMSTREAM_CAMERA_STATUS_OK;
    camstream_camera_stream_config_v1 requested_{};
    camstream_camera_stream_config_v1 active_{};
    std::string backend_path_;
    std::string plugin_path_;
    std::string last_error_;
#ifdef CAMSTREAM_M6_TESTING
    std::function<void()> before_configure_hook_;
#endif
};

} // namespace camstream

#endif
