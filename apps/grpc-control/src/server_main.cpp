#include <camstream/grpc/camera_control_adapter.h>

#define CAMSTREAM_LOG_MODULE "grpc-server"
#include <camstream/common/log.h>

#include <grpcpp/grpcpp.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <utility>

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

bool valid_media_host(const std::string& host)
{
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) != 1)
    {
        return false;
    }
    const std::uint32_t value = ntohl(address.s_addr);
    return value >= UINT32_C(0x01000000) && value < UINT32_C(0xe0000000);
}

bool parse_media_port(const char* text, std::uint16_t& port)
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
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0UL || value > UINT16_MAX)
    {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

void usage(const char* program)
{
    std::fprintf(stderr,
            "usage: %s --backend BACKEND_SO --plugin PLUGIN_SO --source SOURCE "
            "[--listen ADDRESS] [--allow-insecure-lan] "
            "[--media-mode local|rtp-jpeg --media-host IPv4 --media-port UDP_PORT]\n",
            program);
}

} // namespace

int main(int argc, char* argv[])
{
    camstream::CameraControlSettings settings;
    camstream::MediaOutputConfig media;
    std::string address = "127.0.0.1:50051";
    bool allow_insecure_lan = false;
    bool valid_port = true;
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
        else if (index + 1 < argc && std::strcmp(argv[index], "--media-mode") == 0)
        {
            const char* mode = argv[++index];
            if (std::strcmp(mode, "local") == 0)
            {
                media.mode = camstream::MediaOutputMode::Local;
            }
            else if (std::strcmp(mode, "rtp-jpeg") == 0)
            {
                media.mode = camstream::MediaOutputMode::RtpJpeg;
            }
            else
            {
                usage(argv[0]);
                return 2;
            }
        }
        else if (index + 1 < argc && std::strcmp(argv[index], "--media-host") == 0)
        {
            media.host = argv[++index];
        }
        else if (index + 1 < argc && std::strcmp(argv[index], "--media-port") == 0)
        {
            valid_port = parse_media_port(argv[++index], media.port);
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    const bool valid_media = media.mode == camstream::MediaOutputMode::Local
                                     ? media.host.empty() && media.port == 0U
                                     : valid_media_host(media.host) && media.port != 0U;
    if (settings.backend_path.empty() || settings.plugin_path.empty() || settings.source_identifier.empty() || address.size() > 128U ||
            (!loopback_listener(address) && !allow_insecure_lan) || !valid_port || !valid_media)
    {
        usage(argv[0]);
        std::fprintf(stderr, "invalid listener/security or media destination settings\n");
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
        camstream::CameraService service(media);
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
        CAM_LOGI("mono_ms=%llu media mode=%s destination=%s:%u", static_cast<unsigned long long>(monotonic_ms()),
                media.mode == camstream::MediaOutputMode::Local ? "local" : "rtp-jpeg", media.host.empty() ? "none" : media.host.c_str(),
                static_cast<unsigned>(media.port));

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
