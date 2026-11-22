#include <camstream/grpc/camera_control_adapter.h>

#define CAMSTREAM_LOG_MODULE "grpc-server"
#include <camstream/common/log.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <signal.h>
#include <string>

namespace
{

std::uint64_t monotonic_ms()
{
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool loopback_listener(const std::string& address)
{
    return address.rfind("127.0.0.1:", 0U) == 0U || address.rfind("[::1]:", 0U) == 0U;
}

void usage(const char* program)
{
    std::fprintf(stderr,
            "usage: %s --backend BACKEND_SO --plugin PLUGIN_SO --source SOURCE "
            "[--listen ADDRESS] [--allow-insecure-lan]\n",
            program);
}

} // namespace

int main(int argc, char* argv[])
{
    camstream::CameraControlSettings settings;
    std::string address = "127.0.0.1:50051";
    bool allow_insecure_lan = false;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--allow-insecure-lan") == 0)
        {
            allow_insecure_lan = true;
        }
        else if (index + 1 < argc && std::strcmp(argv[index], "--backend") == 0)
        {
            settings.backend_path = argv[++index];
        }
        else if (index + 1 < argc && std::strcmp(argv[index], "--plugin") == 0)
        {
            settings.plugin_path = argv[++index];
        }
        else if (index + 1 < argc && std::strcmp(argv[index], "--source") == 0)
        {
            settings.source_identifier = argv[++index];
        }
        else if (index + 1 < argc && std::strcmp(argv[index], "--listen") == 0)
        {
            address = argv[++index];
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    if (settings.backend_path.empty() || settings.plugin_path.empty() || settings.source_identifier.empty() || address.size() > 128U ||
            (!loopback_listener(address) && !allow_insecure_lan))
    {
        usage(argv[0]);
        std::fprintf(stderr, "non-loopback plaintext requires --allow-insecure-lan\n");
        return 2;
    }

    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0)
    {
        std::fprintf(stderr, "failed to block shutdown signals\n");
        return 1;
    }

    try
    {
        camstream::CameraService service;
        camstream::CameraControlAdapter adapter(service, std::move(settings));
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&adapter);
        builder.SetMaxReceiveMessageSize(4096);
        builder.SetMaxSendMessageSize(65536);
        std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
        if (!server)
        {
            CAM_LOGE("mono_ms=%llu server startup failed listen=%s", static_cast<unsigned long long>(monotonic_ms()), address.c_str());
            return 1;
        }
        CAM_LOGI("mono_ms=%llu server started listen=%s security=plaintext_%s", static_cast<unsigned long long>(monotonic_ms()),
                address.c_str(), loopback_listener(address) ? "loopback" : "trusted_lab_opt_in");

        int signal_number = 0;
        const int signal_result = sigwait(&stop_signals, &signal_number);
        CAM_LOGI("mono_ms=%llu server shutdown signal=%d result=%d", static_cast<unsigned long long>(monotonic_ms()), signal_number,
                signal_result);
        server->Shutdown();
        server->Wait();
        const camstream::CameraServiceResult stopped = service.stop();
        if (!stopped.success)
        {
            CAM_LOGE("mono_ms=%llu service shutdown cleanup failed: %s", static_cast<unsigned long long>(monotonic_ms()),
                    stopped.detail.c_str());
            return 1;
        }
        return signal_result == 0 ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        CAM_LOGE("mono_ms=%llu server exception: %s", static_cast<unsigned long long>(monotonic_ms()), error.what());
        return 1;
    }
}
