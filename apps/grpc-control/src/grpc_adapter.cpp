#include <camstream/grpc/camera_control_adapter.h>

#define CAMSTREAM_LOG_MODULE "grpc-control"
#include <camstream/common/log.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace camstream
{

namespace
{

using namespace control::v1;

constexpr std::uint32_t maximum_dimension = 8192U;
constexpr std::uint32_t maximum_fps_numerator = 240000U;
constexpr std::uint32_t maximum_fps_denominator = 1001U;

std::uint64_t monotonic_ms()
{
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* safe_error(camstream_camera_status_t status)
{
    switch (status)
    {
    case CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT:
        return "invalid camera request";
    case CAMSTREAM_CAMERA_STATUS_INVALID_STATE:
        return "camera state or generation conflict; query GetStatus";
    case CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED:
        return "camera mode is not supported";
    case CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR:
        return "camera resource or device unavailable";
    case CAMSTREAM_CAMERA_STATUS_TIMEOUT:
        return "camera device timed out; query GetStatus";
    default:
        return "camera operation failed; inspect server log";
    }
}

void fill_mode(const camstream_camera_stream_config_v1& source, StreamMode* target)
{
    target->set_width(source.width);
    target->set_height(source.height);
    target->set_pixel_format(source.pixel_format == CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV ? PIXEL_FORMAT_YUYV : PIXEL_FORMAT_UNSPECIFIED);
    target->mutable_frame_rate()->set_numerator(source.frame_rate_numerator);
    target->mutable_frame_rate()->set_denominator(source.frame_rate_denominator);
}

bool valid_mode(const StreamMode& mode)
{
    return mode.pixel_format() == PIXEL_FORMAT_YUYV && mode.width() != 0U && mode.width() <= maximum_dimension && mode.height() != 0U &&
           mode.height() <= maximum_dimension && mode.has_frame_rate() && mode.frame_rate().numerator() != 0U &&
           mode.frame_rate().numerator() <= maximum_fps_numerator && mode.frame_rate().denominator() != 0U &&
           mode.frame_rate().denominator() <= maximum_fps_denominator;
}

CameraState wire_state(CameraServiceState state)
{
    switch (state)
    {
    case CameraServiceState::Idle:
        return CAMERA_STATE_IDLE;
    case CameraServiceState::Configured:
        return CAMERA_STATE_CONFIGURED;
    case CameraServiceState::Streaming:
        return CAMERA_STATE_STREAMING;
    case CameraServiceState::Faulted:
        return CAMERA_STATE_FAULTED;
    }
    return CAMERA_STATE_UNSPECIFIED;
}

CameraOperation wire_operation(CameraServiceOperation operation)
{
    switch (operation)
    {
    case CameraServiceOperation::Configure:
        return CAMERA_OPERATION_CONFIGURE;
    case CameraServiceOperation::Start:
        return CAMERA_OPERATION_START;
    case CameraServiceOperation::Stop:
        return CAMERA_OPERATION_STOP;
    case CameraServiceOperation::Discover:
        return CAMERA_OPERATION_DISCOVER;
    case CameraServiceOperation::None:
        return CAMERA_OPERATION_UNSPECIFIED;
    }
    return CAMERA_OPERATION_UNSPECIFIED;
}

template <typename Action> grpc::Status invoke(const char* operation, grpc::ServerContext* context, Action&& action)
{
    const auto started = std::chrono::steady_clock::now();
    grpc::Status result;
    try
    {
        result = context->IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled before execution") : action();
    }
    catch (const std::exception& error)
    {
        CAM_LOGE("mono_ms=%llu operation=%s exception=%s", static_cast<unsigned long long>(monotonic_ms()), operation, error.what());
        result = grpc::Status(grpc::StatusCode::INTERNAL, "internal camera service error");
    }
    catch (...)
    {
        CAM_LOGE("mono_ms=%llu operation=%s unknown exception", static_cast<unsigned long long>(monotonic_ms()), operation);
        result = grpc::Status(grpc::StatusCode::INTERNAL, "internal camera service error");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (result.ok())
    {
        CAM_LOGI("mono_ms=%llu operation=%s outcome=OK duration_ms=%lld", static_cast<unsigned long long>(monotonic_ms()), operation,
                static_cast<long long>(elapsed.count()));
    }
    else
    {
        CAM_LOGW("mono_ms=%llu operation=%s outcome=%d duration_ms=%lld", static_cast<unsigned long long>(monotonic_ms()), operation,
                static_cast<int>(result.error_code()), static_cast<long long>(elapsed.count()));
    }
    return result;
}

} // namespace

CameraControlAdapter::CameraControlAdapter(CameraService& service, CameraControlSettings settings)
    : service_(service), settings_(std::move(settings))
{
    if (settings_.backend_path.empty() || settings_.plugin_path.empty() || settings_.source_identifier.empty() ||
            settings_.source_identifier.size() >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE)
    {
        throw std::invalid_argument("server camera mapping is incomplete or oversized");
    }
}

#ifdef CAMSTREAM_M6_TESTING
void CameraControlAdapter::set_after_commit_hook(std::function<void(const char*)> hook)
{
    after_commit_hook_ = std::move(hook);
}
#endif

grpc::Status CameraControlAdapter::validate_camera_id(const std::string& camera_id) const
{
    if (camera_id.empty() || camera_id.size() > 32U)
    {
        return {grpc::StatusCode::INVALID_ARGUMENT, "camera_id must be a short nonempty string"};
    }
    if (camera_id != "camera0")
    {
        return {grpc::StatusCode::NOT_FOUND, "logical camera not configured"};
    }
    return grpc::Status::OK;
}

grpc::Status CameraControlAdapter::map_result(const CameraServiceResult& result) const
{
    if (result.success)
    {
        return grpc::Status::OK;
    }
    grpc::StatusCode code = grpc::StatusCode::INTERNAL;
    switch (result.hal_status)
    {
    case CAMSTREAM_CAMERA_STATUS_INVALID_ARGUMENT:
    case CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED:
        code = grpc::StatusCode::INVALID_ARGUMENT;
        break;
    case CAMSTREAM_CAMERA_STATUS_INVALID_STATE:
        code = grpc::StatusCode::FAILED_PRECONDITION;
        break;
    case CAMSTREAM_CAMERA_STATUS_RESOURCE_ERROR:
        code = grpc::StatusCode::UNAVAILABLE;
        break;
    case CAMSTREAM_CAMERA_STATUS_TIMEOUT:
        code = grpc::StatusCode::UNAVAILABLE;
        break;
    default:
        break;
    }
    return {code, safe_error(result.hal_status)};
}

void CameraControlAdapter::fill_status(const CameraServiceStatus& status, CameraStatus* reply) const
{
    reply->set_camera_id("camera0");
    reply->set_state(wire_state(status.state));
    reply->set_stream_active(status.state == CameraServiceState::Streaming);
    reply->set_generation(status.generation);
    reply->set_local_pipeline_frames(status.frames);
    if (status.state != CameraServiceState::Idle && status.requested.width != 0U)
    {
        fill_mode(status.requested, reply->mutable_requested());
    }
    if (status.active.width != 0U)
    {
        fill_mode(status.active, reply->mutable_active());
    }
    LastOperation* last = reply->mutable_last_operation();
    last->set_operation(wire_operation(status.last_operation));
    last->set_success(status.last_operation_success);
    last->set_hal_status(status.last_operation_status);
    if (!status.last_operation_success)
    {
        last->set_error(safe_error(status.last_operation_status));
    }
    if (status.state == CameraServiceState::Faulted)
    {
        reply->set_last_error("camera session fault; inspect server log");
    }
}

grpc::Status CameraControlAdapter::ListCameras(grpc::ServerContext* context, const ListCamerasRequest* request, ListCamerasReply* reply)
{
    (void)request;
    return invoke("ListCameras", context,
            [&]()
            {
                auto* camera = reply->add_cameras();
                camera->set_camera_id("camera0");
                camera->set_source_identifier(settings_.source_identifier);
                return grpc::Status::OK;
            });
}

grpc::Status CameraControlAdapter::GetCapabilities(grpc::ServerContext* context, const CameraRequest* request, GetCapabilitiesReply* reply)
{
    return invoke("GetCapabilities", context,
            [&]()
            {
                const grpc::Status identity = validate_camera_id(request->camera_id());
                if (!identity.ok())
                {
                    return identity;
                }
                const CameraServiceCapabilities capabilities = service_.discover(settings_.backend_path, settings_.source_identifier);
                const grpc::Status result = map_result(capabilities.result);
                if (!result.ok())
                {
                    return result;
                }
                reply->set_camera_id("camera0");
                reply->set_maximum_plane_count(capabilities.maximum_plane_count);
                for (const auto& mode : capabilities.modes)
                {
                    if (mode.pixel_format != CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV)
                    {
                        return grpc::Status(grpc::StatusCode::INTERNAL, "unsupported HAL capability format");
                    }
                    fill_mode(mode, reply->add_modes());
                }
                return grpc::Status::OK;
            });
}

grpc::Status CameraControlAdapter::GetStatus(grpc::ServerContext* context, const CameraRequest* request, CameraStatus* reply)
{
    return invoke("GetStatus", context,
            [&]()
            {
                const grpc::Status identity = validate_camera_id(request->camera_id());
                if (!identity.ok())
                {
                    return identity;
                }
                fill_status(service_.status(), reply);
                return grpc::Status::OK;
            });
}

grpc::Status CameraControlAdapter::ConfigureCamera(grpc::ServerContext* context, const ConfigureCameraRequest* request, CameraStatus* reply)
{
    return invoke("ConfigureCamera", context,
            [&]()
            {
                const grpc::Status identity = validate_camera_id(request->camera_id());
                if (!identity.ok())
                {
                    return identity;
                }
                if (request->generation_precondition_case() != ConfigureCameraRequest::kExpectedGeneration)
                {
                    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "expected_generation is required");
                }
                if (!request->has_requested() || !valid_mode(request->requested()))
                {
                    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid YUYV dimensions or rational frame rate");
                }
                camstream_camera_stream_config_v1 mode{};
                mode.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
                mode.struct_size = sizeof(mode);
                std::memcpy(mode.source_identifier, settings_.source_identifier.c_str(), settings_.source_identifier.size() + 1U);
                mode.width = request->requested().width();
                mode.height = request->requested().height();
                mode.pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
                mode.frame_rate_numerator = request->requested().frame_rate().numerator();
                mode.frame_rate_denominator = request->requested().frame_rate().denominator();

                const CameraServiceResult result =
                        service_.configure(settings_.backend_path, settings_.plugin_path, mode, request->expected_generation());
                const grpc::Status mapped = map_result(result);
                if (!mapped.ok())
                {
                    return mapped;
                }
                CAM_LOGI("mono_ms=%llu camera=camera0 operation=ConfigureCamera generation=%llu state=configured",
                        static_cast<unsigned long long>(monotonic_ms()),
                        static_cast<unsigned long long>(result.committed_status->generation));
#ifdef CAMSTREAM_M6_TESTING
                if (after_commit_hook_)
                {
                    after_commit_hook_("ConfigureCamera");
                }
#endif
                if (context->IsCancelled())
                {
                    return grpc::Status(grpc::StatusCode::CANCELLED, "configuration committed; query GetStatus");
                }
                fill_status(*result.committed_status, reply);
                return grpc::Status::OK;
            });
}

grpc::Status CameraControlAdapter::StartStream(grpc::ServerContext* context, const TransitionRequest* request, CameraStatus* reply)
{
    return invoke("StartStream", context,
            [&]()
            {
                const grpc::Status identity = validate_camera_id(request->camera_id());
                if (!identity.ok())
                {
                    return identity;
                }
                if (request->generation_precondition_case() != TransitionRequest::kExpectedGeneration)
                {
                    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "expected_generation is required");
                }
                const CameraServiceResult result = service_.start(request->expected_generation());
                const grpc::Status mapped = map_result(result);
                if (!mapped.ok())
                {
                    return mapped;
                }
                CAM_LOGI("mono_ms=%llu camera=camera0 operation=StartStream generation=%llu state=streaming",
                        static_cast<unsigned long long>(monotonic_ms()),
                        static_cast<unsigned long long>(result.committed_status->generation));
#ifdef CAMSTREAM_M6_TESTING
                if (after_commit_hook_)
                {
                    after_commit_hook_("StartStream");
                }
#endif
                if (context->IsCancelled())
                {
                    return grpc::Status(grpc::StatusCode::CANCELLED, "start committed; query GetStatus");
                }
                fill_status(*result.committed_status, reply);
                return grpc::Status::OK;
            });
}

grpc::Status CameraControlAdapter::StopStream(grpc::ServerContext* context, const TransitionRequest* request, CameraStatus* reply)
{
    return invoke("StopStream", context,
            [&]()
            {
                const grpc::Status identity = validate_camera_id(request->camera_id());
                if (!identity.ok())
                {
                    return identity;
                }
                if (request->generation_precondition_case() != TransitionRequest::kExpectedGeneration)
                {
                    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "expected_generation is required");
                }
                const CameraServiceResult result = service_.stop(request->expected_generation());
                const grpc::Status mapped = map_result(result);
                if (!mapped.ok())
                {
                    return mapped;
                }
                CAM_LOGI("mono_ms=%llu camera=camera0 operation=StopStream generation=%llu state=idle",
                        static_cast<unsigned long long>(monotonic_ms()),
                        static_cast<unsigned long long>(result.committed_status->generation));
                if (context->IsCancelled())
                {
                    return grpc::Status(grpc::StatusCode::CANCELLED, "stop committed; query GetStatus");
                }
                fill_status(*result.committed_status, reply);
                return grpc::Status::OK;
            });
}

} // namespace camstream
