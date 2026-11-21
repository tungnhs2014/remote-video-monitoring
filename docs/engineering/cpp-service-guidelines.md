# C++17 Application, CameraService & GStreamer Controller

**Scope:** Process-level service, pipeline orchestration, gRPC adapters and C interop.  
**Coding baseline:** **C++17** in this project even where learning curriculum is C++20. Do not introduce `std::span`, concepts or other C++20-only API into production.

## 1. Tại sao service dùng C++, low-level dùng C?

Chọn ngôn ngữ theo responsibility, không theo niềm tin C luôn nhanh hơn C++:

- **C11 HAL/backends**: stable C ABI, explicit pointer/resource semantics, ops table, Linux V4L2 I/O, easy C consumers.
- **C++17 service**: tổ chức long-lived objects, RAII, strongly typed state, lifecycle composition, work queues khi cần, gRPC C++ SDK integration.
- **GStreamer**: GLib/GObject C API ở plugin; controller C++ vẫn có thể sử dụng thư viện C bằng wrapper có ownership rõ.

**C ABI không đồng nghĩa implementation bắt buộc C**, nhưng người dùng **đã chọn** migration HAL/backends thành C11 để rèn chuyên sâu Embedded C. Không phá boundary này chỉ vì C++ có RAII.

## 2. Object lifetime và RAII thực dụng

RAII: resource acquisition gắn với object lifetime; constructor chỉ tạo trạng thái hợp lệ (hoặc thất bại theo policy), destructor giải phóng resource được sở hữu khi còn hợp lệ. Sử dụng `std::unique_ptr` cho exclusive C++ object ownership, `std::shared_ptr` chỉ khi thật sự cần shared lifetime và đã kiểm soát cycles.

Các handle C cần custom deleter:

```cpp
struct GstObjectUnref {
    void operator()(GstElement* element) const noexcept
    {
        if (element != nullptr) {
            gst_object_unref(element);
        }
    }
};

using OwnedElement = std::unique_ptr<GstElement, GstObjectUnref>;
```

**Minh họa**, không sử dụng wrapper mù quáng: khi `gst_bin_add` nhận ownership, phải `release()` hoặc làm ownership transfer tương ứng để tránh double-unref; typed GstBuffer/GstCaps có cleanup APIs tương ứng, không phải tất cả đều `gst_object_unref`.

RAII không miễn trừ việc thiết kế shutdown order. C++ owner phải giữ HAL/module hợp lệ qua cleanup callbacks theo [system contract §4–5](../architecture/system-contracts.md). Nếu callback context trỏ tới `this`, stop/unregister/await callback completion trước khi object bị destroy. Không áp destructor/RAII machinery này vào pure-C HAL implementation.

## 3. Copy/Move/Reference/Pointer — contract rõ

- `T`: pass by value khi ownership/value copy có ý nghĩa và chi phí hợp lý.
- `const T&`: read-only borrowed access trong lifetime của caller, không lưu reference vượt thời hạn.
- `T&`: non-null mutable borrowed access; không tự tạo ownership.
- `T*`: mặc định dùng cho nullable borrowed access trong C++ interfaces của dự án, nhưng type tự nó không biểu diễn ownership; C APIs có thể trả owned pointers và cần contract/deleter rõ.
- `std::unique_ptr<T>`: exclusive ownership transfer.
- `std::shared_ptr<T>`: shared ownership (không tự giải quyết data race trong `T`).

**Shallow copy** của class chứa raw FD/handle tạo hai owner và double-close/double-free. Với resource-owning types: thường delete copy, cho move nếu meaningful, hoặc implement deep copy khi contract thật sự yêu cầu. Rule of Zero/Rule of Five ưu tiên clear semantics; copy-and-swap là một lựa chọn khi class có real value semantics, không là pattern mặc định cho camera FD.

`std::move` chỉ cast sang rvalue category; không bảo đảm resource thật sự được move nếu type không có move operation thích hợp. Moved-from object vẫn phải destructible/valid theo class invariant.

**Lifetime caveat:** `const T&` không làm dữ liệu bất biến với thread khác; `std::string_view`/borrowed span/reference không được sống lâu hơn storage.

## 4. Encapsulation, invariant, composition và polymorphism

CameraService có thể giữ:

- một `ServiceState` rõ ràng;
- object điều phối `PipelineController`;
- session handle/state (khi có active streaming session);
- gRPC server/control adapter;
- synchronization primitives với lock boundaries documented.

Định nghĩa invariant: chỉ một acquisition owner và không có competing capture path; service sống khi stream stop; mỗi transition trả status và active configuration chỉ sau khi thành công. Theo quyết định M4, CameraService/session sở hữu HAL instance và backend module client reference; GStreamerController sở hữu pipeline/bus, còn `camstreamsrc` chỉ mượn HAL. Stop phải quiesce source trước khi destroy HAL và unload module.

Prefer composition khi service sở hữu controller. Không dùng class inheritance nhiều tầng hoặc runtime polymorphism cho module chỉ có một implementation và không cần virtual dispatch. Interface C++ chỉ tạo khi có multiple implementations/test seam thật; tránh duplicate Camera HAL backend abstraction ở tầng C++.

`override`/virtual destructor khi polymorphic base thực sự được delete qua base pointer. Không xây `CameraBackendCpp` thứ hai chỉ để wrap V4L2 khi HAL C đã cung cấp abstraction.

## 5. C/C++ interop và exception policy

- C public header bảo vệ declarations bằng `extern "C"` khi biên dịch bằng C++, nhưng `.c` compile dưới C11; không đưa C++ types, exceptions hoặc STL qua C ABI.
- `extern "C"` kiểm soát language linkage của function/symbol; không bảo đảm object representation/layout tương đương cho mọi C++ class.
- Không cast bất kỳ pointer thành `camstream_camera_instance*` trừ representation/lifetime conversion được implementation contract chứng minh.
- C callbacks giữ nguyên declared signatures, trả status khi signature định nghĩa status (destroy vẫn void); không có C++ exception đi xuyên C ABI.
- Nếu compatibility stage còn C++ backend, every C-boundary callback catch `std::bad_alloc`/generic exceptions, map sang public statuses và preserve cleanup. Pure C11 target không dùng `try/catch`.
- Theo approved exception policy, không thêm C++ bridge vào HAL C11 để bắt exceptions từ callbacks vi phạm contract; xem [system contract §4.1](../architecture/system-contracts.md#41-abi-and-exception-policy--preserved-baseline).
- Không trả pointer tới C++ `std::string` buffer hoặc temporary `c_str()` nếu caller còn sử dụng sau object destruction.

## 6. Service process lifetime vs streaming session lifetime

Phải phân biệt:

| Lifetime | Khi nào tạo | Khi nào kết thúc |
|---|---|---|
| Process/gRPC server | Process startup | Graceful shutdown |
| CameraService | Process initialization | Process teardown |
| Streaming session | Approved Start/Configure flow | Stop, failure recovery, shutdown |
| GStreamer pipeline | Session create | Session teardown |
| HAL instance/backend reference | CameraService/session acquires during Configure | Stop completes source quiescence and token cleanup, then destroys HAL before final module unload |
| Copy-mode GstBuffer | Source copies an acquired frame | GStreamer-owned memory release; HAL token resolved before publication |
| Wrapped memory (OPTIONAL) | Approved lease/bridge, not an existing HAL retain API | Final downstream memory owner triggers serialized release/cleanup through the lease |

Một-shot CLI `main` chạy pipeline rồi exit khác **long-lived service** nhận nhiều Start/Stop RPC. Không giữ code one-shot mà tuyên bố remote service hỗ trợ nhiều sessions.

### Historical M0-B state proposal and M5 implementation

`Idle → Starting → Streaming → Stopping → Idle`, thêm `Faulted/Recovering` nếu cần. `Configure` khi idle/configured phải theo contract rõ. Mỗi RPC:

1. Acquire state ownership/short mutex.
2. Validate current state/request; reserve transition.
3. Release mutex; perform backend/pipeline operation potentially blocking.
4. Re-acquire short lock; commit or rollback state if generation/session still matches.
5. Prepare the authoritative result/active config; transmission or client receipt can still fail, so GetStatus supports reconciliation.

State change needs cancellation/stop handling for race: concurrent Start+Start, Start+Stop, Configure+Start, Stop+GetStatus, device lost during Start. Có thể dùng serialized executor/command queue nếu state machine cần, **nhưng chưa mặc định bắt buộc**. Không giữ lock xuyên `gst_element_set_state` hoặc callback có thể re-enter để tránh deadlock.

The steps above record the historical M0-B state reservation sketch, not the M5 implementation. Dropping the state lock and checking a generation afterward does not serialize HAL operations or make concurrent teardown safe. M5 serializes public control operations with an operation mutex and relies on completed pipeline quiescence before HAL teardown; source callbacks never acquire that mutex.

The [M0-B finalization proposal](../architecture/system-contracts.md#61-m0-b-finalization-proposal--owner-approval-pending) adds `Configuring` and `Configured` to the service-visible transition model. CameraService owns the HAL context during Configure and retains it after success through Stop/fault cleanup; Stop closes that context and another Start requires Configure. Reserve one mutating transition under the short service lock. Stop may record cancellation intent during Starting without running a concurrent HAL operation; Start must observe it and roll back before Streaming commit. One operation gate admits at most one same-instance HAL caller, including service Configure/Start/Stop and source wait/acquire/release; it tracks source ownership through checked release/cleanup. The gate's mutex is not held while invoking HAL or GStreamer callbacks. Stop closes source admission, requests controller flushing, then waits for bounded in-flight completion before any HAL cleanup. A `GstBaseSrc::unlock_stop` during teardown cannot reopen admission. The controller must complete pipeline shutdown before service destroys the camera context. State-valid cleanup skips HAL stop in Configured and skips close after a failed stop in Started; preserve the failure and perform best-effort destroy after quiescence before final module release. If bounded completion fails, retain pipeline/source and HAL contexts and report the fault rather than racing destruction. No mandatory queue, executor or eventfd follows from this rule.

## 7. Threading, callback và stop

GStreamer streaming/bus threads và gRPC worker threads có thể song song. C++ mutex bảo vệ *service state*, không tự bảo vệ HAL token maps. Có lock order và ownership:

- Who owns gRPC server shutdown?
- Who handles GstBus ERROR/EOS while an RPC Stop is pending?
- Who requests flushing and waits for bounded HAL wait/acquire to finish without concurrent close/destroy?
- In copy-first mode, who resolves copy/release failure? In optional wrap mode only, who keeps the lease until downstream returns its token?
- Who is allowed to call camera `destroy` and `dlclose`?
- What if client times out after Start already committed?

`std::condition_variable` phải wait cùng predicate; `notify_one` không chứng minh condition đúng. Thread join/destruction order rõ. Avoid detached threads for resource-owning workflows. Shutdown request idempotency policy phải phân biệt "Stop already stopped" và "Stop still in progress".

## 8. Errors, diagnostics, performance

Runtime path trả structured status/result (có message/detail để log), không bắt lỗi rồi đổi hết thành success. Error service levels:

- validation/state conflict;
- no camera/unsupported config;
- backend resources/device I/O;
- GStreamer pipeline negotiation/flow error;
- network/receiver issue;
- deadline/cancellation/service shutdown.

Exceptions được phép trong **internal C++** nếu phù hợp style, nhưng service boundary phải map consistent status và cleanup. Không dùng exception cho normal expected device disconnect nếu C HAL đã dùng status. C++ destructor nên `noexcept`; không throw khi cleanup.

Đo service startup, RPC latency/p99, RAM, binary size + libraries (gRPC deps có thể lớn), media CPU/memory; đừng tối ưu bằng chuyển class thành C function vô căn cứ.

## 9. Tests/code review gate

- [ ] Existing C headers include sạch trong C++17 translation unit; ABI tests pass.
- [ ] No duplicated backend/resource model at C++ layer.
- [ ] RAII wrappers có đúng transfer-of-ownership và custom deleters.
- [ ] Resource-owning C++ classes không vô ý copy raw handles.
- [ ] Start/Stop/Configure/GetStatus concurrency có status transitions và rollback.
- [ ] Long-lived service vẫn nhận RPC sau một session Stop.
- [ ] Shutdown stops new work, cancels wait, drains callbacks/tokens, unloads backend last.
- [ ] No lock held across unknown/backend/GStreamer callback.
- [ ] Exceptions không đi xuyên C ABI.
- [ ] Tests include concurrent RPCs, disconnect, repeated start-stop, error injection.
- [ ] C++17 compiler/build baseline; không dùng C++20-only library API.

## 10. Curriculum mapping

Author-reported source index: C++ V10 M03 function interfaces, M04 pointers/references/callbacks/lifetime, M05 classes/special members/value semantics, M06 inheritance/polymorphism. Access/read evidence for original chapters is recorded in [provenance](../../SOURCE_EVIDENCE.md); do not assume the index proves access to them. Apply to C++ service **selectively**; production compatibility is C++17, not the reported C++20 teaching baseline.

Avoid teaching-only excess: custom smart pointer, special-member demonstrations in production types, inheritance hierarchy for every element, manual frame-copy class when GStreamer/HAL API already owns it.

