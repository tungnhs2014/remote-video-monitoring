#include <camstream/service/camera_service.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <string>
#include <thread>

namespace
{
thread_local bool fail_next_allocation = false;
}

void* operator new(std::size_t size)
{
    if (fail_next_allocation)
    {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size))
    {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{

bool expect(bool condition, const char* step)
{
    if (!condition)
    {
        std::fprintf(stderr, "M5 service test failed: %s\n", step);
    }
    return condition;
}

camstream_camera_stream_config_v1 request(const char* source, std::uint32_t width, std::uint32_t height)
{
    camstream_camera_stream_config_v1 config{};
    config.abi_version = CAMSTREAM_CAMERA_ABI_VERSION_V1;
    config.struct_size = sizeof(config);
    std::memcpy(config.source_identifier, source, std::strlen(source) + 1U);
    config.width = width;
    config.height = height;
    config.pixel_format = CAMSTREAM_CAMERA_PIXEL_FORMAT_YUYV;
    config.frame_rate_numerator = 30U;
    config.frame_rate_denominator = 1U;
    return config;
}

bool wait_for(camstream::CameraService& service, std::uint64_t frames, bool fault_expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do
    {
        const auto status = service.status();
        if (fault_expected && status.state == camstream::CameraServiceState::Faulted)
        {
            return true;
        }
        if (!fault_expected && status.state == camstream::CameraServiceState::Streaming && status.frames >= frames)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

struct FaultHooks
{
    void* module = nullptr;
    bool (*wait_active)() = nullptr;
    void (*release_wait)() = nullptr;
    bool (*release_failed)() = nullptr;
};

FaultHooks load_fault_hooks(const std::string& path)
{
    FaultHooks hooks;
    hooks.module = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (hooks.module == nullptr)
    {
        return hooks;
    }
    void* active = dlsym(hooks.module, "camstream_test_wait_active");
    void* release = dlsym(hooks.module, "camstream_test_release_wait");
    void* failed = dlsym(hooks.module, "camstream_test_release_failed");
    static_assert(sizeof(active) == sizeof(hooks.wait_active), "POSIX dlsym pointer width");
    std::memcpy(&hooks.wait_active, &active, sizeof(active));
    std::memcpy(&hooks.release_wait, &release, sizeof(release));
    std::memcpy(&hooks.release_failed, &failed, sizeof(failed));
    return hooks;
}

template <typename Predicate> bool wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        return 2;
    }
    const std::string simulated = argv[1];
    const std::string faults = argv[2];
    const std::string plugin = argv[3];
    const auto simulated_request = request("simulated0", 64U, 48U);
    camstream::CameraService service;

    if (!expect(!service.configure("/missing/backend.so", plugin, simulated_request).success, "missing backend") ||
            !expect(service.status().state == camstream::CameraServiceState::Idle, "load rollback"))
    {
        return 1;
    }
    for (unsigned cycle = 0U; cycle < 3U; ++cycle)
    {
        if (!expect(service.configure(simulated, plugin, simulated_request).success, "configure") ||
                !expect(service.configure(simulated, plugin, simulated_request).success, "idempotent configure") ||
                !expect(!service.configure(simulated, plugin, request("other", 64U, 48U)).success,
                        "different configure rejected") ||
                !expect(service.status().active.width == 64U, "active configuration") ||
                !expect(service.start().success, "start") ||
                !expect(!service.start().success, "duplicate start rejected") ||
                !expect(wait_for(service, 3U, false), "frame delivery") || !expect(service.stop().success, "stop") ||
                !expect(service.stop().success, "repeated stop") ||
                !expect(service.status().state == camstream::CameraServiceState::Idle, "idle after stop"))
        {
            return 1;
        }
    }

    const auto unsupported_request = request("unsupported-active", 2U, 2U);
    const std::uint64_t generation_before_rejection = service.status().generation;
    const auto unsupported_result = service.configure(faults, plugin, unsupported_request);
    const auto rejected_status = service.status();
    if (!expect(!unsupported_result.success && unsupported_result.hal_status == CAMSTREAM_CAMERA_STATUS_NOT_SUPPORTED,
                "unsupported active mode rejected during Configure") ||
            !expect(rejected_status.state == camstream::CameraServiceState::Idle &&
                        rejected_status.generation == generation_before_rejection,
                    "unsupported active mode rolls back without generation change") ||
            !expect(!service.start().success, "Start requires a committed configuration") ||
            !expect(service.configure(simulated, plugin, simulated_request).success, "Configure after active mode rollback") ||
            !expect(service.start().success && service.stop().success, "Start/Stop after active mode rollback"))
    {
        return 1;
    }

    if (!expect(service.configure(simulated, plugin, simulated_request).success, "exception test configure") ||
            !expect(service.start().success, "exception test start"))
    {
        return 1;
    }
    bool start_threw = false;
    fail_next_allocation = true;
    try
    {
        (void)service.start();
    }
    catch (const std::bad_alloc&)
    {
        start_threw = true;
    }
    fail_next_allocation = false;
    bool configure_threw = false;
    fail_next_allocation = true;
    try
    {
        (void)service.configure(simulated, plugin, simulated_request);
    }
    catch (const std::bad_alloc&)
    {
        configure_threw = true;
    }
    fail_next_allocation = false;
    if (!expect(start_threw && configure_threw, "rejected-call allocation faults injected") ||
            !expect(wait_for(service, 3U, false), "stream survives rejected-call exceptions") ||
            !expect(service.stop().success, "exception test stop"))
    {
        return 1;
    }

    if (!expect(service.configure(simulated, "/missing/plugin.so", simulated_request).success,
                "configure before plugin failure") ||
            !expect(!service.start().success, "plugin start failure") ||
            !expect(service.status().state == camstream::CameraServiceState::Idle, "plugin failure rollback"))
    {
        return 1;
    }

    if (!expect(service.configure(faults, plugin, request("gated-release", 2U, 2U)).success, "gated fault configure"))
    {
        return 1;
    }
    FaultHooks hooks = load_fault_hooks(faults);
    if (!expect(hooks.module != nullptr && hooks.wait_active != nullptr && hooks.release_wait != nullptr &&
                        hooks.release_failed != nullptr,
                "fault seam loaded") ||
            !expect(service.start().success, "gated fault start") ||
            !expect(wait_until(hooks.wait_active), "fault wait entered"))
    {
        return 1;
    }
    hooks.release_wait();
    if (!expect(wait_until(hooks.release_failed), "release fault injected"))
    {
        return 1;
    }
    const auto fault_stop = service.stop(); // No preceding status poll may consume the bus error.
    if (!expect(!fault_stop.success, "Stop reports queued source error") ||
            !expect(service.status().state == camstream::CameraServiceState::Idle, "fault cleanup idle"))
    {
        return 1;
    }
    dlclose(hooks.module);

    if (!expect(service.configure(faults, plugin, request("blocked-wait", 2U, 2U)).success, "blocked wait configure"))
    {
        return 1;
    }
    hooks = load_fault_hooks(faults);
    if (!expect(hooks.module != nullptr && hooks.wait_active != nullptr && hooks.release_wait != nullptr,
                "blocked wait seam loaded") ||
            !expect(service.start().success, "blocked wait start") ||
            !expect(wait_until(hooks.wait_active), "blocked callback entered"))
    {
        return 1;
    }
    std::atomic<bool> stop_entered{false};
    std::atomic<bool> stopped{false};
    bool stop_ok = false;
    std::thread stopper(
            [&service, &stop_entered, &stopped, &stop_ok]()
            {
                stop_entered.store(true, std::memory_order_release);
                stop_ok = service.stop().success;
                stopped.store(true, std::memory_order_release);
            });
    const bool entered = wait_until([&stop_entered]() { return stop_entered.load(std::memory_order_acquire); });
    const bool quiescence = wait_until([&service]() { return service.quiescence_started_for_test(); });
    const bool waited_for_callback = entered && quiescence && !stopped.load(std::memory_order_acquire);
    hooks.release_wait();
    stopper.join();
    dlclose(hooks.module);
    if (!expect(waited_for_callback, "Stop awaited in-flight callback") ||
            !expect(stop_ok, "quiescent Stop succeeded") ||
            !expect(service.status().state == camstream::CameraServiceState::Idle, "blocked cleanup idle"))
    {
        return 1;
    }

    std::puts("M5 service lifecycle and fault regressions: PASS");
    return 0;
}
