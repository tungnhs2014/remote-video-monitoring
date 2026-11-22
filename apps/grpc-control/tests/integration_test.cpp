#include <camstream/grpc/camera_control_adapter.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{

using namespace camstream::control::v1;

bool expect(bool condition, const char* step)
{
    if (!condition)
    {
        std::fprintf(stderr, "M6 integration failed: %s\n", step);
    }
    return condition;
}

struct Fixture
{
    camstream::CameraService service;
    camstream::CameraControlAdapter adapter;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<CameraControl::Stub> client;

    explicit Fixture(camstream::CameraControlSettings settings) : adapter(service, std::move(settings))
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&adapter);
        builder.SetMaxReceiveMessageSize(4096);
        builder.SetMaxSendMessageSize(65536);
        server = builder.BuildAndStart();
        if (server && port != 0)
        {
            channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
            client = CameraControl::NewStub(channel);
        }
    }

    ~Fixture()
    {
        if (server)
        {
            server->Shutdown();
            server->Wait();
        }
        (void)service.stop();
    }
};

void deadline(grpc::ClientContext& context)
{
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
}

grpc::Status get_status(Fixture& fixture, CameraStatus& reply, const char* camera_id = "camera0")
{
    CameraRequest request;
    request.set_camera_id(camera_id);
    grpc::ClientContext context;
    deadline(context);
    return fixture.client->GetStatus(&context, request, &reply);
}

grpc::Status configure(Fixture& fixture, std::uint64_t generation, CameraStatus& reply, std::uint32_t width = 64U,
        std::uint32_t height = 48U, std::uint32_t fps_numerator = 30U, std::uint32_t fps_denominator = 1U)
{
    ConfigureCameraRequest request;
    request.set_camera_id("camera0");
    request.set_expected_generation(generation);
    StreamMode* mode = request.mutable_requested();
    mode->set_width(width);
    mode->set_height(height);
    mode->set_pixel_format(PIXEL_FORMAT_YUYV);
    mode->mutable_frame_rate()->set_numerator(fps_numerator);
    mode->mutable_frame_rate()->set_denominator(fps_denominator);
    grpc::ClientContext context;
    deadline(context);
    return fixture.client->ConfigureCamera(&context, request, &reply);
}

grpc::Status transition(Fixture& fixture, bool start, std::uint64_t generation, CameraStatus& reply)
{
    TransitionRequest request;
    request.set_camera_id("camera0");
    request.set_expected_generation(generation);
    grpc::ClientContext context;
    deadline(context);
    return start ? fixture.client->StartStream(&context, request, &reply) : fixture.client->StopStream(&context, request, &reply);
}

bool wait_for_frames(Fixture& fixture, std::uint64_t minimum)
{
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < limit)
    {
        CameraStatus status;
        if (!get_status(fixture, status).ok() || status.state() == CAMERA_STATE_FAULTED)
        {
            return false;
        }
        if (status.local_pipeline_frames() >= minimum)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct ReplyGate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;

    void wait_after_commit(const char*)
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this]() { return released; });
    }

    bool await_entry()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(3), [this]() { return entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
};

bool lost_reply_reconciliation(Fixture& fixture, bool start, std::uint64_t generation, bool expire_deadline = false)
{
    ReplyGate gate;
    fixture.adapter.set_after_commit_hook([&gate](const char* operation) { gate.wait_after_commit(operation); });
    CameraStatus reply;
    grpc::Status result;
    grpc::ClientContext context;
    if (expire_deadline)
    {
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
    }
    else
    {
        deadline(context);
    }
    std::thread caller(
            [&]()
            {
                if (start)
                {
                    TransitionRequest request;
                    request.set_camera_id("camera0");
                    request.set_expected_generation(generation);
                    result = fixture.client->StartStream(&context, request, &reply);
                }
                else
                {
                    ConfigureCameraRequest request;
                    request.set_camera_id("camera0");
                    request.set_expected_generation(generation);
                    auto* mode = request.mutable_requested();
                    mode->set_width(64U);
                    mode->set_height(48U);
                    mode->set_pixel_format(PIXEL_FORMAT_YUYV);
                    mode->mutable_frame_rate()->set_numerator(30U);
                    mode->mutable_frame_rate()->set_denominator(1U);
                    result = fixture.client->ConfigureCamera(&context, request, &reply);
                }
            });

    const bool committed = gate.await_entry();
    CameraStatus observed;
    const bool visible = committed && get_status(fixture, observed).ok() && observed.generation() == generation + 1U &&
                         observed.state() == (start ? CAMERA_STATE_STREAMING : CAMERA_STATE_CONFIGURED);
    if (expire_deadline)
    {
        caller.join(); // The committed operation is held until gRPC reports its deadline.
        gate.release();
    }
    else
    {
        context.TryCancel();
        gate.release();
        caller.join();
    }
    fixture.adapter.set_after_commit_hook({});
    CameraStatus reconciled;
    const bool durable = get_status(fixture, reconciled).ok() && reconciled.generation() == generation + 1U &&
                         reconciled.state() == (start ? CAMERA_STATE_STREAMING : CAMERA_STATE_CONFIGURED);
    return expect(visible && durable && (expire_deadline ? result.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED : !result.ok()),
            expire_deadline ? "deadline after commit preserved configuration"
                            : (start ? "lost Start reply preserved commit" : "lost Configure reply preserved commit"));
}

bool test_basic(Fixture& fixture)
{
    CameraStatus reply;
    if (!expect(get_status(fixture, reply).ok() && reply.state() == CAMERA_STATE_IDLE && reply.generation() == 0U, "initial status"))
    {
        return false;
    }
    ListCamerasRequest list_request;
    ListCamerasReply list_reply;
    grpc::ClientContext list_context;
    deadline(list_context);
    if (!expect(fixture.client->ListCameras(&list_context, list_request, &list_reply).ok() && list_reply.cameras_size() == 1 &&
                        list_reply.cameras(0).camera_id() == "camera0" && list_reply.cameras(0).source_identifier() == "simulated0",
                "server logical camera mapping"))
    {
        return false;
    }
    CameraRequest selector;
    selector.set_camera_id("camera0");
    GetCapabilitiesReply capabilities;
    grpc::ClientContext capabilities_context;
    deadline(capabilities_context);
    if (!expect(fixture.client->GetCapabilities(&capabilities_context, selector, &capabilities).ok() && capabilities.modes_size() == 1 &&
                        capabilities.modes(0).width() == 64U && capabilities.modes(0).pixel_format() == PIXEL_FORMAT_YUYV,
                "real HAL capabilities"))
    {
        return false;
    }
    if (!expect(get_status(fixture, reply, "camera1").error_code() == grpc::StatusCode::NOT_FOUND, "unknown ID") ||
            !expect(get_status(fixture, reply, "").error_code() == grpc::StatusCode::INVALID_ARGUMENT, "empty ID"))
    {
        return false;
    }

    ConfigureCameraRequest invalid;
    invalid.set_camera_id("camera0");
    invalid.set_expected_generation(0U);
    invalid.mutable_requested()->set_width(64U);
    invalid.mutable_requested()->set_height(48U);
    invalid.mutable_requested()->set_pixel_format(static_cast<PixelFormat>(99));
    invalid.mutable_requested()->mutable_frame_rate()->set_numerator(30U);
    invalid.mutable_requested()->mutable_frame_rate()->set_denominator(1U);
    grpc::ClientContext invalid_context;
    deadline(invalid_context);
    if (!expect(fixture.client->ConfigureCamera(&invalid_context, invalid, &reply).error_code() == grpc::StatusCode::INVALID_ARGUMENT,
                "unknown pixel enum"))
    {
        return false;
    }
    invalid.clear_expected_generation();
    grpc::ClientContext missing_generation_context;
    deadline(missing_generation_context);
    if (!expect(fixture.client->ConfigureCamera(&missing_generation_context, invalid, &reply).error_code() ==
                        grpc::StatusCode::INVALID_ARGUMENT,
                "missing generation precondition"))
    {
        return false;
    }
    if (!expect(configure(fixture, 0U, reply, 8193U).error_code() == grpc::StatusCode::INVALID_ARGUMENT, "oversized width") ||
            !expect(configure(fixture, 0U, reply, 64U, 48U, 0U).error_code() == grpc::StatusCode::INVALID_ARGUMENT, "zero FPS") ||
            !expect(configure(fixture, 0U, reply, 32U, 48U).error_code() == grpc::StatusCode::INVALID_ARGUMENT, "unsupported HAL mode") ||
            !expect(configure(fixture, 0U, reply, 64U, 48U, 15U).error_code() == grpc::StatusCode::INVALID_ARGUMENT, "unsupported HAL FPS"))
    {
        return false;
    }

    if (!expect(configure(fixture, 0U, reply).ok() && reply.state() == CAMERA_STATE_CONFIGURED && reply.generation() == 1U &&
                        reply.has_active() && reply.active().width() == 64U,
                "valid Configure"))
    {
        return false;
    }
    if (!expect(configure(fixture, 1U, reply).ok() && reply.generation() == 1U, "identical Configure") ||
            !expect(configure(fixture, 1U, reply, 64U, 48U, 15U).error_code() == grpc::StatusCode::FAILED_PRECONDITION,
                    "different Configure while Configured"))
    {
        return false;
    }
    grpc::ClientContext busy_context;
    deadline(busy_context);
    if (!expect(fixture.client->GetCapabilities(&busy_context, selector, &capabilities).error_code() ==
                        grpc::StatusCode::FAILED_PRECONDITION,
                "capability discovery while Configured"))
    {
        return false;
    }
    if (!expect(transition(fixture, true, 1U, reply).ok() && reply.state() == CAMERA_STATE_STREAMING && reply.generation() == 2U, "Start"))
    {
        return false;
    }
    if (!expect(transition(fixture, true, 2U, reply).error_code() == grpc::StatusCode::FAILED_PRECONDITION, "duplicate Start") ||
            !expect(configure(fixture, 2U, reply).error_code() == grpc::StatusCode::FAILED_PRECONDITION, "Configure during Streaming") ||
            !expect(transition(fixture, false, 1U, reply).error_code() == grpc::StatusCode::FAILED_PRECONDITION, "stale Stop") ||
            !expect(wait_for_frames(fixture, 3U), "local frame observation"))
    {
        return false;
    }
    if (!expect(transition(fixture, false, 2U, reply).ok() && reply.state() == CAMERA_STATE_IDLE && reply.generation() == 3U, "Stop") ||
            !expect(transition(fixture, false, 3U, reply).ok() && reply.generation() == 3U, "Stop from Idle"))
    {
        return false;
    }
    return true;
}

bool test_concurrency(Fixture& fixture)
{
    CameraStatus reply;
    if (!expect(configure(fixture, 3U, reply).ok() && reply.generation() == 4U, "configure concurrent Start"))
    {
        return false;
    }
    std::atomic<bool> go{false};
    grpc::Status first;
    grpc::Status second;
    CameraStatus first_reply;
    CameraStatus second_reply;
    auto invoke_start = [&](grpc::Status& result, CameraStatus& target)
    {
        while (!go.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        result = transition(fixture, true, 4U, target);
    };
    std::thread first_thread(invoke_start, std::ref(first), std::ref(first_reply));
    std::thread second_thread(invoke_start, std::ref(second), std::ref(second_reply));
    go.store(true, std::memory_order_release);
    first_thread.join();
    second_thread.join();
    if (!expect(first.ok() != second.ok(), "one concurrent Start commits") ||
            !expect((first.ok() ? second : first).error_code() == grpc::StatusCode::FAILED_PRECONDITION, "other Start rejected") ||
            !expect(get_status(fixture, reply).ok() && reply.state() == CAMERA_STATE_STREAMING && reply.generation() == 5U,
                    "single streaming owner") ||
            !expect(transition(fixture, false, 5U, reply).ok() && reply.generation() == 6U, "cleanup concurrent Start"))
    {
        return false;
    }

    if (!expect(configure(fixture, 6U, reply).ok() && reply.generation() == 7U, "configure Start Stop race"))
    {
        return false;
    }
    go.store(false, std::memory_order_release);
    grpc::Status start_result;
    grpc::Status stop_result;
    auto invoke_transition = [&](bool start, grpc::Status& result)
    {
        CameraStatus target;
        while (!go.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        result = transition(fixture, start, 7U, target);
    };
    std::thread starter(invoke_transition, true, std::ref(start_result));
    std::thread stopper(invoke_transition, false, std::ref(stop_result));
    go.store(true, std::memory_order_release);
    starter.join();
    stopper.join();
    if (!expect(start_result.ok() != stop_result.ok(), "Start Stop generation race has one winner") ||
            !expect(get_status(fixture, reply).ok() && reply.generation() == 8U, "race generation advanced"))
    {
        return false;
    }
    if (reply.state() == CAMERA_STATE_STREAMING)
    {
        return expect(transition(fixture, false, 8U, reply).ok() && reply.state() == CAMERA_STATE_IDLE, "cleanup Start winner");
    }
    return expect(reply.state() == CAMERA_STATE_IDLE, "Stop winner left Idle");
}

bool test_status_during_transition(Fixture& fixture)
{
    const std::uint64_t generation = fixture.service.status().generation;
    ReplyGate gate;
    fixture.service.set_before_configure_hook([&gate]() { gate.wait_after_commit("before Configure"); });
    CameraStatus configured;
    grpc::Status configure_result;
    std::thread configure_thread([&]() { configure_result = configure(fixture, generation, configured); });
    const bool entered = gate.await_entry();
    CameraStatus observed;
    auto status_result = std::async(std::launch::async, [&]() { return get_status(fixture, observed); });
    const bool blocked = entered && status_result.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    gate.release();
    configure_thread.join();
    fixture.service.set_before_configure_hook({});
    const bool completed =
            status_result.get().ok() && observed.state() == CAMERA_STATE_CONFIGURED && observed.generation() == generation + 1U;
    CameraStatus stopped;
    const bool cleaned = transition(fixture, false, generation + 1U, stopped).ok();
    return expect(blocked && configure_result.ok() && completed && cleaned, "GetStatus waits for in-progress Configure");
}

bool test_committed_reply_snapshot(Fixture& fixture)
{
    const std::uint64_t generation = fixture.service.status().generation;
    ReplyGate gate;
    fixture.adapter.set_after_commit_hook([&gate](const char* operation) { gate.wait_after_commit(operation); });
    CameraStatus configured;
    grpc::Status configure_result;
    std::thread configure_thread([&]() { configure_result = configure(fixture, generation, configured); });
    const bool committed = gate.await_entry();
    CameraStatus stopped;
    const bool stop_result = committed && transition(fixture, false, generation + 1U, stopped).ok();
    gate.release();
    configure_thread.join();
    fixture.adapter.set_after_commit_hook({});
    return expect(committed && stop_result && configure_result.ok() && configured.state() == CAMERA_STATE_CONFIGURED &&
                          configured.generation() == generation + 1U && stopped.state() == CAMERA_STATE_IDLE &&
                          stopped.generation() == generation + 2U,
            "successful Configure reply retains its commit snapshot across concurrent Stop");
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        return 2;
    }
    Fixture fixture({argv[1], argv[2], "simulated0"});
    if (!expect(fixture.client != nullptr, "loopback server started") || !test_basic(fixture))
    {
        return 1;
    }
    if (!test_concurrency(fixture))
    {
        return 1;
    }
    if (!test_status_during_transition(fixture) || !test_committed_reply_snapshot(fixture))
    {
        return 1;
    }
    const std::uint64_t reconciliation_generation = fixture.service.status().generation;
    if (!lost_reply_reconciliation(fixture, false, reconciliation_generation) ||
            !lost_reply_reconciliation(fixture, true, reconciliation_generation + 1U))
    {
        return 1;
    }
    CameraStatus reply;
    if (!expect(transition(fixture, false, reconciliation_generation + 2U, reply).ok() &&
                        reply.generation() == reconciliation_generation + 3U,
                "stop after lost Start"))
    {
        return 1;
    }
    const std::uint64_t deadline_generation = fixture.service.status().generation;
    if (!lost_reply_reconciliation(fixture, false, deadline_generation, true) ||
            !expect(transition(fixture, false, deadline_generation + 1U, reply).ok(), "stop after expired Configure reply"))
    {
        return 1;
    }

    const std::uint64_t generation = fixture.service.status().generation;
    if (!expect(configure(fixture, generation, reply).ok(), "configure for shutdown") ||
            !expect(transition(fixture, true, reply.generation(), reply).ok(), "start for shutdown"))
    {
        return 1;
    }
    fixture.server->Shutdown();
    fixture.server->Wait();
    fixture.server.reset();
    if (!expect(fixture.service.stop().success && fixture.service.status().state == camstream::CameraServiceState::Idle,
                "shutdown active session"))
    {
        return 1;
    }

    Fixture unavailable({"/missing/backend.so", argv[2], "simulated0"});
    CameraRequest selector;
    selector.set_camera_id("camera0");
    GetCapabilitiesReply capabilities;
    grpc::ClientContext context;
    deadline(context);
    if (!expect(unavailable.client != nullptr && unavailable.client->GetCapabilities(&context, selector, &capabilities).error_code() ==
                                                         grpc::StatusCode::UNAVAILABLE,
                "backend failure maps to UNAVAILABLE"))
    {
        return 1;
    }

    std::puts("M6 real gRPC loopback integration: PASS");
    return 0;
}
