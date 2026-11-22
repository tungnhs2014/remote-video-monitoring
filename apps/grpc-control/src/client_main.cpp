#include "camera_control.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{

using namespace camstream::control::v1;

void usage(const char* program)
{
    std::fprintf(stderr,
            "usage: %s [--server HOST:PORT] [--camera camera0] "
            "list|status|capabilities|configure WIDTH HEIGHT FPS_NUM FPS_DEN|start|stop\n",
            program);
}

bool parse_positive(const char* text, std::uint32_t& value)
{
    if (text == nullptr || *text == '\0' || *text == '-')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0UL || parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

void print_mode(const StreamMode& mode)
{
    std::printf("%ux%u format=%s fps=%u/%u", mode.width(), mode.height(), PixelFormat_Name(mode.pixel_format()).c_str(),
            mode.frame_rate().numerator(), mode.frame_rate().denominator());
}

void print_status(const CameraStatus& status)
{
    std::printf("camera=%s state=%s generation=%llu local_pipeline_frames=%llu stream_active=%s\n", status.camera_id().c_str(),
            CameraState_Name(status.state()).c_str(), static_cast<unsigned long long>(status.generation()),
            static_cast<unsigned long long>(status.local_pipeline_frames()), status.stream_active() ? "yes" : "no");
    if (status.has_requested())
    {
        std::printf("requested: ");
        print_mode(status.requested());
        std::putchar('\n');
    }
    if (status.has_active())
    {
        std::printf("active: ");
        print_mode(status.active());
        std::putchar('\n');
    }
    if (status.has_last_operation())
    {
        const auto& last = status.last_operation();
        std::printf("last_operation=%s success=%s hal_status=%d error=%s\n", CameraOperation_Name(last.operation()).c_str(),
                last.success() ? "yes" : "no", last.hal_status(), last.error().c_str());
    }
    if (!status.last_error().empty())
    {
        std::printf("last_error=%s\n", status.last_error().c_str());
    }
}

void set_deadline(grpc::ClientContext& context)
{
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
}

bool print_rpc_error(const grpc::Status& status)
{
    if (status.ok())
    {
        return false;
    }
    std::fprintf(stderr, "gRPC status=%d detail=%s; query status before retrying a mutating command\n",
            static_cast<int>(status.error_code()), status.error_message().c_str());
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string server_address = "127.0.0.1:50051";
    std::string camera_id = "camera0";
    int index = 1;
    while (index + 1 < argc)
    {
        if (std::strcmp(argv[index], "--server") == 0)
        {
            server_address = argv[index + 1];
            index += 2;
        }
        else if (std::strcmp(argv[index], "--camera") == 0)
        {
            camera_id = argv[index + 1];
            index += 2;
        }
        else
        {
            break;
        }
    }
    if (index >= argc || server_address.empty() || server_address.size() > 128U || camera_id.size() > 32U)
    {
        usage(argv[0]);
        return 2;
    }

    const std::string command = argv[index++];
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    std::unique_ptr<CameraControl::Stub> stub = CameraControl::NewStub(channel);
    CameraRequest selector;
    selector.set_camera_id(camera_id);

    if (command == "list" && index == argc)
    {
        ListCamerasRequest request;
        ListCamerasReply reply;
        grpc::ClientContext context;
        set_deadline(context);
        const grpc::Status status = stub->ListCameras(&context, request, &reply);
        if (print_rpc_error(status))
        {
            return 1;
        }
        for (const auto& camera : reply.cameras())
        {
            std::printf("camera=%s source=%s (server mapping; availability is not implied)\n", camera.camera_id().c_str(),
                    camera.source_identifier().c_str());
        }
        return 0;
    }
    if (command == "status" && index == argc)
    {
        CameraStatus reply;
        grpc::ClientContext context;
        set_deadline(context);
        const grpc::Status status = stub->GetStatus(&context, selector, &reply);
        if (print_rpc_error(status))
        {
            return 1;
        }
        print_status(reply);
        return 0;
    }
    if (command == "capabilities" && index == argc)
    {
        GetCapabilitiesReply reply;
        grpc::ClientContext context;
        set_deadline(context);
        const grpc::Status status = stub->GetCapabilities(&context, selector, &reply);
        if (print_rpc_error(status))
        {
            return 1;
        }
        std::printf("camera=%s maximum_planes=%u modes=%d\n", reply.camera_id().c_str(), reply.maximum_plane_count(), reply.modes_size());
        for (const auto& mode : reply.modes())
        {
            print_mode(mode);
            std::putchar('\n');
        }
        return 0;
    }

    if (command != "configure" && command != "start" && command != "stop")
    {
        usage(argv[0]);
        return 2;
    }
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t fps_numerator = 0U;
    std::uint32_t fps_denominator = 0U;
    if (command == "configure")
    {
        if (argc - index != 4 || !parse_positive(argv[index], width) || !parse_positive(argv[index + 1], height) ||
                !parse_positive(argv[index + 2], fps_numerator) || !parse_positive(argv[index + 3], fps_denominator))
        {
            usage(argv[0]);
            return 2;
        }
    }
    else if (index != argc)
    {
        usage(argv[0]);
        return 2;
    }

    CameraStatus before;
    grpc::ClientContext status_context;
    set_deadline(status_context);
    const grpc::Status before_status = stub->GetStatus(&status_context, selector, &before);
    if (print_rpc_error(before_status))
    {
        return 1;
    }

    CameraStatus reply;
    grpc::ClientContext command_context;
    set_deadline(command_context);
    grpc::Status result;
    if (command == "configure")
    {
        ConfigureCameraRequest request;
        request.set_camera_id(camera_id);
        request.set_expected_generation(before.generation());
        StreamMode* mode = request.mutable_requested();
        mode->set_width(width);
        mode->set_height(height);
        mode->set_pixel_format(PIXEL_FORMAT_YUYV);
        mode->mutable_frame_rate()->set_numerator(fps_numerator);
        mode->mutable_frame_rate()->set_denominator(fps_denominator);
        result = stub->ConfigureCamera(&command_context, request, &reply);
    }
    else
    {
        TransitionRequest request;
        request.set_camera_id(camera_id);
        request.set_expected_generation(before.generation());
        result = command == "start" ? stub->StartStream(&command_context, request, &reply)
                                    : stub->StopStream(&command_context, request, &reply);
    }
    if (print_rpc_error(result))
    {
        return 1;
    }
    print_status(reply);
    return 0;
}
