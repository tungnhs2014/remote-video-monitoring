#ifndef CAMSTREAM_GRPC_CAMERA_CONTROL_ADAPTER_H
#define CAMSTREAM_GRPC_CAMERA_CONTROL_ADAPTER_H

#include <camstream/service/camera_service.h>

#include "camera_control.grpc.pb.h"

#ifdef CAMSTREAM_M6_TESTING
#include <functional>
#endif
#include <string>

namespace camstream
{

/** Operator-selected server mapping; no RPC client chooses these paths or source identifier. */
struct CameraControlSettings
{
    std::string backend_path;
    std::string plugin_path;
    std::string source_identifier;
};

/** Implements the single-camera control RPCs for logical ID camera0.
 * Borrows one process-owned CameraService; the server drains handlers before destroying that service.
 * Mutating calls require an expected generation. A lost/cancelled reply does not undo a committed operation;
 * clients reconcile through GetStatus. Media payload never crosses this interface.
 */
class CameraControlAdapter final : public control::v1::CameraControl::Service
{
  public:
    /** Validates the operator mapping; throws std::invalid_argument for missing or oversized fields. */
    CameraControlAdapter(CameraService& service, CameraControlSettings settings);

    grpc::Status ListCameras(
            grpc::ServerContext* context, const control::v1::ListCamerasRequest* request, control::v1::ListCamerasReply* reply) override;
    grpc::Status GetCapabilities(
            grpc::ServerContext* context, const control::v1::CameraRequest* request, control::v1::GetCapabilitiesReply* reply) override;
    grpc::Status GetStatus(
            grpc::ServerContext* context, const control::v1::CameraRequest* request, control::v1::CameraStatus* reply) override;
    grpc::Status ConfigureCamera(
            grpc::ServerContext* context, const control::v1::ConfigureCameraRequest* request, control::v1::CameraStatus* reply) override;
    grpc::Status StartStream(
            grpc::ServerContext* context, const control::v1::TransitionRequest* request, control::v1::CameraStatus* reply) override;
    grpc::Status StopStream(
            grpc::ServerContext* context, const control::v1::TransitionRequest* request, control::v1::CameraStatus* reply) override;

#ifdef CAMSTREAM_M6_TESTING
    void set_after_commit_hook(std::function<void(const char*)> hook);
#endif

  private:
    grpc::Status validate_camera_id(const std::string& camera_id) const;
    grpc::Status map_result(const CameraServiceResult& result) const;
    void fill_status(const CameraServiceStatus& status, control::v1::CameraStatus* reply) const;

    CameraService& service_;
    CameraControlSettings settings_;
#ifdef CAMSTREAM_M6_TESTING
    std::function<void(const char*)> after_commit_hook_;
#endif
};

} // namespace camstream

#endif
