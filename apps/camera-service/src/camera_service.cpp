#include <camstream/service/camera_service.h>

#define CAMSTREAM_LOG_MODULE "camera-service"
#include <camstream/common/log.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace camstream
{

namespace
{

bool same_request(const camstream_camera_stream_config_v1& left, const camstream_camera_stream_config_v1& right)
{
    return std::strcmp(left.source_identifier, right.source_identifier) == 0 && left.width == right.width &&
           left.height == right.height && left.pixel_format == right.pixel_format &&
           left.frame_rate_numerator == right.frame_rate_numerator &&
           left.frame_rate_denominator == right.frame_rate_denominator;
}

CameraServiceResult success()
{
    return {true, CAMSTREAM_CAMERA_STATUS_OK, {}};
}

} // namespace

CameraService::~CameraService() noexcept
{
    try
    {
        const CameraServiceResult result = stop();
        if (!result.success)
        {
            CAM_LOGE("shutdown cleanup failed: %s", result.detail.c_str());
            if (controller_.active())
            {
                std::abort(); // Cannot free a camera while source callbacks may still borrow it.
            }
        }
    }
    catch (...)
    {
        std::abort();
    }
}

CameraServiceResult CameraService::hal_failure(const char* operation, camstream_camera_status_t code) const
{
    char detail[256] = {0};
    if (camera_ == nullptr)
    {
        (void)camstream_camera_hal_get_last_error(detail, sizeof(detail));
    }
    else
    {
        (void)camstream_camera_get_last_error(camera_, detail, sizeof(detail));
    }
    return {false, code, std::string(operation) + " failed: " + detail};
}

CameraServiceResult CameraService::configure(const std::string& backend_path, const std::string& plugin_path,
        const camstream_camera_stream_config_v1& requested)
{
    std::lock_guard<std::mutex> operation(operation_mutex_);
    const bool rollback_on_exception = state_ == CameraServiceState::Idle && !module_loaded_;
    try
    {
        return configure_locked(backend_path, plugin_path, requested);
    }
    catch (...)
    {
        if (rollback_on_exception)
        {
            try
            {
                (void)stop_locked();
            }
            catch (...)
            {
                // Ownership stays in the service for a later cleanup attempt.
            }
        }
        throw;
    }
}

CameraServiceResult CameraService::configure_locked(const std::string& backend_path, const std::string& plugin_path,
        const camstream_camera_stream_config_v1& requested)
{
    if (backend_path.empty() || plugin_path.empty() ||
            std::memchr(requested.source_identifier, '\0', sizeof(requested.source_identifier)) == nullptr ||
            requested.source_identifier[0] == '\0' || requested.abi_version != CAMSTREAM_CAMERA_ABI_VERSION_V1 ||
            requested.struct_size < sizeof(requested))
    {
        return {false, CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT, "invalid backend, plugin or camera request"};
    }
    if (state_ == CameraServiceState::Configured && backend_path == backend_path_ && plugin_path == plugin_path_ &&
            same_request(requested, requested_))
    {
        return success();
    }
    if (state_ != CameraServiceState::Idle || module_loaded_)
    {
        return {false, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "Configure requires Idle or an identical configuration"};
    }

    backend_path_ = backend_path;
    plugin_path_ = plugin_path;
    requested_ = requested;
    camstream_camera_status_t code = camstream_camera_hal_load_backend(backend_path_.c_str());
    if (code != CAMSTREAM_CAMERA_STATUS_OK)
    {
        const CameraServiceResult result = hal_failure("load_backend", code);
        last_error_ = result.detail;
        return result;
    }
    module_loaded_ = true;

    code = camstream_camera_create(&camera_);
    if (code == CAMSTREAM_CAMERA_STATUS_OK)
    {
        code = camstream_camera_open(camera_, requested.source_identifier);
        if (code == CAMSTREAM_CAMERA_STATUS_OK)
        {
            opened_ = true;
            active_ = {};
            active_.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
            active_.struct_size = sizeof(active_);
            code = camstream_camera_configure(camera_, &requested_, &active_);
        }
    }
    if (code != CAMSTREAM_CAMERA_STATUS_OK)
    {
        const CameraServiceResult primary = hal_failure("Configure", code);
        const CameraServiceResult cleanup = stop_locked();
        last_error_ = primary.detail;
        if (!cleanup.success)
        {
            last_error_ += "; cleanup: " + cleanup.detail;
        }
        CAM_LOGE("%s", last_error_.c_str());
        return {false, primary.hal_status, last_error_};
    }

    state_ = CameraServiceState::Configured;
    last_error_.clear();
    CAM_LOGI("configured source=%s active=%ux%u fps=%u/%u", active_.source_identifier, active_.width, active_.height,
            active_.frame_rate_numerator, active_.frame_rate_denominator);
    return success();
}

CameraServiceResult CameraService::start()
{
    std::lock_guard<std::mutex> operation(operation_mutex_);
    const bool rollback_on_exception = state_ == CameraServiceState::Configured && camera_ != nullptr;
    try
    {
        return start_locked();
    }
    catch (...)
    {
        if (rollback_on_exception)
        {
            try
            {
                (void)stop_locked();
            }
            catch (...)
            {
                // Ownership stays in the service for a later cleanup attempt.
            }
        }
        throw;
    }
}

CameraServiceResult CameraService::start_locked()
{
    if (state_ != CameraServiceState::Configured || camera_ == nullptr)
    {
        return {false, CAMSTREAM_CAMERA_STATUS_INVALID_STATE, "Start requires Configured"};
    }
    const camstream_camera_status_t code = camstream_camera_start(camera_);
    if (code != CAMSTREAM_CAMERA_STATUS_OK)
    {
        const CameraServiceResult result = hal_failure("start", code);
        state_ = CameraServiceState::Faulted;
        last_error_ = result.detail;
        return result;
    }
    started_ = true;

    std::string error;
    if (!controller_.start(camera_, active_, plugin_path_, error))
    {
        const CameraServiceResult cleanup = stop_locked();
        last_error_ = "pipeline start failed: " + error;
        if (!cleanup.success)
        {
            last_error_ += "; cleanup: " + cleanup.detail;
        }
        CAM_LOGE("%s", last_error_.c_str());
        return {false, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, last_error_};
    }
    state_ = CameraServiceState::Streaming;
    last_error_.clear();
    return success();
}

CameraServiceResult CameraService::stop_locked()
{
    std::string pipeline_error;
    if (!controller_.stop(pipeline_error))
    {
        state_ = CameraServiceState::Faulted;
        last_error_ = pipeline_error;
        return {false, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, pipeline_error};
    }

    CameraServiceResult first = success();
    if (!pipeline_error.empty())
    {
        first = {false, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, pipeline_error};
    }
    else if (state_ == CameraServiceState::Faulted && !last_error_.empty())
    {
        first = {false, CAMSTREAM_CAMERA_STATUS_INTERNAL_ERROR, last_error_};
    }
    if (camera_ != nullptr)
    {
        if (started_)
        {
            const camstream_camera_status_t code = camstream_camera_stop(camera_);
            if (code != CAMSTREAM_CAMERA_STATUS_OK)
            {
                if (first.success)
                {
                    first = hal_failure("stop", code);
                }
            }
            else
            {
                started_ = false;
            }
        }
        if (!started_ && opened_)
        {
            const camstream_camera_status_t code = camstream_camera_close(camera_);
            if (code != CAMSTREAM_CAMERA_STATUS_OK && first.success)
            {
                first = hal_failure("close", code);
            }
        }
        camstream_camera_destroy(camera_); // Best effort after all source callbacks have quiesced.
        camera_ = nullptr;
        opened_ = false;
        started_ = false;
    }
    if (module_loaded_)
    {
        const camstream_camera_status_t code = camstream_camera_hal_unload_backend();
        if (code != CAMSTREAM_CAMERA_STATUS_OK)
        {
            if (first.success)
            {
                first = hal_failure("unload_backend", code);
            }
        }
        else
        {
            module_loaded_ = false;
        }
    }
    state_ = module_loaded_ ? CameraServiceState::Faulted : CameraServiceState::Idle;
    active_ = {};
    if (!first.success)
    {
        last_error_ = first.detail;
    }
    return first;
}

CameraServiceResult CameraService::stop()
{
    std::lock_guard<std::mutex> operation(operation_mutex_);
    return stop_locked();
}

CameraServiceStatus CameraService::status()
{
    std::lock_guard<std::mutex> operation(operation_mutex_);
    if (state_ == CameraServiceState::Streaming)
    {
        std::string bus_error;
        if (!controller_.poll_bus(bus_error))
        {
            state_ = CameraServiceState::Faulted;
            last_error_ = bus_error;
        }
    }
    return {state_, active_, controller_.frames(), last_error_};
}

} // namespace camstream
