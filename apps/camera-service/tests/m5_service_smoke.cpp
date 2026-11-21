#include <camstream/service/camera_service.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{

bool parse_positive(const char* text, std::uint32_t& value)
{
    if (text == nullptr || *text == '\0' || *text == '-')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0UL || parsed > UINT32_MAX)
    {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 10)
    {
        std::fprintf(
                stderr, "usage: %s BACKEND_SO PLUGIN_SO SOURCE WIDTH HEIGHT FPS_NUM FPS_DEN FRAMES CYCLES\n", argv[0]);
        return 2;
    }
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t fps_num = 0U;
    std::uint32_t fps_den = 0U;
    std::uint32_t frames = 0U;
    std::uint32_t cycles = 0U;
    if (std::strlen(argv[3]) >= CAMSTREAM_CAMERA_MAX_SOURCE_ID_SIZE || !parse_positive(argv[4], width) ||
            !parse_positive(argv[5], height) || !parse_positive(argv[6], fps_num) ||
            !parse_positive(argv[7], fps_den) || !parse_positive(argv[8], frames) || !parse_positive(argv[9], cycles) ||
            cycles > 5U || frames > 300U)
    {
        std::fprintf(stderr, "invalid source or mode arguments\n");
        return 2;
    }

    camstream_camera_stream_config_v1 request{};
    request.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    request.struct_size = sizeof(request);
    std::memcpy(request.source_identifier, argv[3], std::strlen(argv[3]) + 1U);
    request.width = width;
    request.height = height;
    request.pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    request.frame_rate_numerator = fps_num;
    request.frame_rate_denominator = fps_den;

    camstream::CameraService service;
    for (std::uint32_t cycle = 1U; cycle <= cycles; ++cycle)
    {
        const auto configured = service.configure(argv[1], argv[2], request);
        if (!configured.success)
        {
            std::fprintf(stderr, "Configure cycle=%u failed: %s\n", cycle, configured.detail.c_str());
            return 1;
        }
        const auto active = service.status().active;
        std::printf("ACTIVE cycle=%u source=%s width=%u height=%u fps=%u/%u\n", cycle, active.source_identifier,
                active.width, active.height, active.frame_rate_numerator, active.frame_rate_denominator);
        const auto started = service.start();
        if (!started.success)
        {
            std::fprintf(stderr, "Start cycle=%u failed: %s\n", cycle, started.detail.c_str());
            return 1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        camstream::CameraServiceStatus observed{};
        do
        {
            observed = service.status();
            if (observed.state == camstream::CameraServiceState::Faulted || observed.frames >= frames)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);

        const auto stopped = service.stop();
        std::printf("SUMMARY cycle=%u frames=%llu requested=%u stop=%s\n", cycle,
                static_cast<unsigned long long>(observed.frames), frames, stopped.success ? "PASS" : "FAIL");
        if (observed.state == camstream::CameraServiceState::Faulted || observed.frames < frames || !stopped.success)
        {
            std::fprintf(
                    stderr, "Session cycle=%u failed: %s %s\n", cycle, observed.detail.c_str(), stopped.detail.c_str());
            return 1;
        }
    }
    std::printf("RESULT PASS cycles=%u\n", cycles);
    return 0;
}
