# GStreamer API & Custom `camstreamsrc` — Implementation Guidelines

**Scope:** GStreamer pipeline/controller và custom source element đọc frames từ Camera HAL.  
**Non-goal:** Viết lại V4L2 driver, encoder, RTP stack hoặc GStreamer core.

`camstreamsrc` is implemented in C/GObject; the future GStreamerController is C++17. The later M4 ownership decision, recorded in [system contracts §4–5](../architecture/system-contracts.md), places the HAL instance and backend module client reference in CameraService/session. GStreamerController owns pipeline state, bus and coordinated teardown; `camstreamsrc` borrows HAL access. GStreamer reference counting does not own the HAL session.

## 1. Mental model — các object không thay thế nhau

| Thành phần | Khái niệm | Ví dụ trong project |
|---|---|---|
| `GstElement` | Component xử lý/trao đổi buffer/event | `camstreamsrc`, `videoconvert`, encoder, sink |
| `GstPad` | Kết nối hướng dữ liệu giữa elements | src pad của source tới sink pad kế tiếp |
| `GstCaps` | Media format contract | `video/x-raw,format=YUY2,width=...,height=...,framerate=...` |
| `GstBuffer` | Ref-counted carrier của media data + metadata | Một camera frame |
| `GstMemory` | Storage bên trong GstBuffer | Copy hoặc wrapped HAL buffer |
| `GstBus` | Message path từ pipeline tới application | ERROR, EOS, STATE_CHANGED |
| `GstClock` | Thời gian để pipeline đồng bộ | Buffer running time/PTS |
| `GstBin` / `GstPipeline` | Composition, clock/state aggregation | Service-managed media graph |

Một `GstBuffer` không đồng nghĩa một malloc block. `GstBuffer` có references/metadata/multiple memories; `gst_buffer_unref` giảm refcount, memory finalizer có thể chạy khi ref cuối cùng mất.

## 2. Object model và ownership — phải hiểu trước code

GStreamer/GObject có reference counting; một số API nhận **transfer of ownership**, số khác trả borrowed/full reference. Tra API cụ thể; **không** khái quát rằng tất cả `gst_*_new()` giống nhau về floating ref. Khi `gst_bin_add` chuyển ownership element vào bin, không tự unref theo pattern của một full-ref khác mà không kiểm tra transfer semantics; khi fail add/link, cần cleanup đúng chủ.

Key rules:

- `gst_object_ref` và `gst_object_unref` phải theo source of truth ownership.
- `gst_buffer_ref/unref` quyết định lifetime của GstBuffer, không tự động release HAL frame nếu chưa gắn custom memory/free callback phù hợp.
- `gst_buffer_map` với `GST_MAP_READ/WRITE` cần `gst_buffer_unmap` đối xứng; không dùng `map.data` sau unmap.
- `gst_caps_unref`, `gst_sample_unref`, `gst_message_unref`, `g_error_free` theo actual ownership contract.
- Với ERROR/WARNING parsing: free owned `GError` bằng `g_error_free()`, owned debug string bằng `g_free()`, và owned bus message bằng `gst_message_unref()` trên mọi nhánh; borrowed message source không tự unref nếu chưa lấy thêm reference. Xem transfer annotation của API, không unref borrowed objects.
- `gst_element_set_state` trả `GstStateChangeReturn`; `ASYNC` không có nghĩa pipeline đã đạt target ngay; cần async completion/state observation.
- `gst_pad_link`/element linking success khác caps negotiation runtime success.

**Câu review bắt buộc:** Downstream memory là bản copy hay đang borrow HAL storage? Với wrap tùy chọn, ai giữ last memory reference và notify chạy trên thread nào?

## 3. `camstreamsrc` có nhiệm vụ gì?

Source plugin là **bridge**; approved copy-first flow:

```text
GStreamer asks source for buffer
  → bounded HAL wait, then acquire frame under caller serialization
  → validate HAL frame/token/active configuration
  → copy into GstBuffer-owned memory and attach metadata
  → successfully release HAL frame token
  → transfer GstBuffer to pipeline
  → downstream final memory release frees only the copy
```

Plugin **không**:

- open `/dev/videoX` hoặc gọi `VIDIOC_S_FMT/REQBUFS/DQBUF`;
- giữ private backend header; biết driver buffer indices/token semantics ngoài HAL;
- quản lý network sockets của gRPC;
- tự tạo một camera acquisition path song song với V4L2 backend.
- tạo/hủy HAL instance hoặc giữ backend module client reference.

The implemented `camstreamsrc` subclasses `GstPushSrc` and uses `GstBaseSrc` lifecycle, caps and unlock behavior. For later source designs, choose a subclass from its required behavior and the available GStreamer API, not its name alone.

The original M0-B text left HAL owner placement open. The later M4 decision assigns its owning lifetime to CameraService/session; `camstreamsrc` borrows the public HAL instance. The controller owns pipeline state, bus and coordinated teardown. The session and source context must outlive relevant in-flight calls, with same-instance HAL calls serialized. GObject dispose/finalize cleanup does not replace coordinated stop; a raw source pointer in an asynchronous notify is not a lifetime guarantee.

## 4. Negotiation — camera requested, driver active, GStreamer caps

Hai negotiation liên tiếp nhưng không trùng:

1. HAL/V4L2: request YUYV/resolution/frame interval → driver reports active configuration.
2. GStreamer: source advertises/presents caps phản ánh dữ liệu **thực**, downstream chấp nhận hoặc yêu cầu transform.

Không tự advertise MJPEG vì webcam hỗ trợ MJPEG nếu HAL v1 chỉ YUYV. V4L2 `V4L2_PIX_FMT_YUYV` thường map tới GStreamer `YUY2`, không map ngẫu nhiên sang I420. `bytesperline` và padding có thể khác tightly-packed image; GstVideoMeta/GstVideoInfo hoặc copy/transform phải diễn đạt stride thật. Chọn code căn cứ GStreamer API đã kiểm tra, không invent caps fields.

**Frame interval:** Không hardcode `30/1` từ requested config nếu driver không xác nhận active FPS. Nếu chưa có confirmed interval, phải quyết định representation/negotiation policy. Timestamp/frame duration cần có rule riêng.

The [M0-B finalization proposal](../architecture/system-contracts.md#61-m0-b-finalization-proposal--owner-approval-pending) uses pipeline running time sampled as close to dequeue/acquire as possible for initial live-source PTS. This is a capture-time approximation: frame availability and dequeue can lag exposure, and the HAL timestamp has no verified mapping to the selected GstClock. Do not copy absolute HAL monotonic nanoseconds into PTS. A validated clock-domain/latency mapping can be considered later. Derive nominal duration from the driver-confirmed active interval, not from measured delivery; mark the first frame after Start and detected sequence gaps with `GST_BUFFER_FLAG_DISCONT`. Reset the time origin for a new session. Decide caps negotiation/set-caps/start ordering and representation of stride/color/field metadata using fields actually available in HAL; do not silently extend ABI. Negotiated framerate is not measured delivered FPS.

<a id="buffer-lifetimes"></a>

## 5. Hai chiến lược `GstBuffer` — không tuyên bố zero-copy sớm

### 5.1 Copy mode — approved baseline

- HAL acquire frame.
- Allocate GstBuffer memory đủ bytes, copy row-by-row/stride-aware.
- HAL release token sau copy, trước publication; only successful release resolves ownership. Nếu release fail, không publish normal success, giữ mapping/pending cleanup theo HAL contract và báo controlled failure.
- Downstream chỉ giữ GstBuffer-owned memory.

**Lifetime:** HAL instance phải sống trong wait/acquire/copy/release và mọi cleanup retry. Sau release thành công, downstream copy không còn phụ thuộc camera/module/mapping; finalizer không gọi HAL. Allocation/copy validation fail vẫn phải attempt release; nếu release đó fail, owner không được biến mất.

**Ưu:** backend buffer không bị giữ lâu; stop/unload đơn giản. **Nhược:** thêm CPU/memory-bandwidth cost, cần đo trên BBB. Copy-first không đòi triển khai wrap để so benchmark trước khi được chấp nhận.

### 5.2 Wrap/zero-copy candidate — cần contract mạnh hơn

- Wrap external HAL payload thành `GstMemory`/`GstBuffer` với destruction notify.
- Notify context phải giữ frame ownership và một application-owned lifetime lease cần được phê duyệt riêng. Public HAL hiện không có camera retain/unref API; không invent API hoặc thay ABI ngầm.
- Camera/module/mapping không được destroy/unmap trong lúc còn wrapped buffers.
- Frame token không bị release sớm sau `create`.
- Notify có thể chạy trên downstream thread; bridge phải serialize HAL release với operations khác và giữ cleanup ownership nếu release fail. Một notify attempt không chứng minh token đã được release thành công. Không trực tiếp gọi HAL song song với wait/acquire/stop/destroy.

Wrap-only timeline: `HAL acquire → wrap/transfer external memory → final memory owner notifies bridge → serialized HAL release → backend may reuse storage`. Lease, cancellation and shutdown barriers must be approved before implementing wrap. No wrap/zero-copy feature is required by the initial copy path.

Hàm như `gst_buffer_new_wrapped_full` hỗ trợ lifetime notify, nhưng **wrapping không tự bảo đảm zero copy end-to-end**. Downstream có thể convert/copy; encode/network vẫn có chi phí. Nếu cần DMA-BUF, phải xem device support, buffer type, caps features, allocator/ownership và benchmark. Đừng thêm DMA-BUF chỉ vì IPC đã học.

### 5.3 Failure matrix

| Failure | Cleanup cần làm |
|---|---|
 HAL acquire thất bại | Không xuất GstBuffer; map lỗi HAL thành `GstFlowReturn` có ý nghĩa |
 Buffer allocation thất bại | Attempt HAL release; retain cleanup ownership and report error if release fails |
 Caps validation fail | Reject negotiation; resolve any acquired token through checked release/cleanup |
 Copy-mode GstBuffer published, pipeline stops | Downstream owns independent copy; no HAL finalizer dependency |
 Wrapped GstBuffer published, pipeline stops (optional) | Lease/barrier protects HAL storage until final memory release and successful HAL cleanup |
 Duplicate/invalid token | HAL reject và chuyển appropriate fault/status, không tự QBUF |
 Wrap notify after stop request (optional) | Keep lease/bridge alive; no dangling camera pointer or concurrent HAL teardown |

## 6. Lifecycle và state machine GStreamer

`NULL ↔ READY ↔ PAUSED ↔ PLAYING` là GStreamer state transitions. Trên live source, behavior `NO_PREROLL`, `ASYNC`, `unlock`/cancel blocking `create` cần được xử lý theo class contract. Không đồng nhất trạng thái `PLAYING` với "đã nhận frame" hay "remote viewer đã xem được video".

Source lifecycle under the later M4 ownership decision; remaining M5 service coordination details below are guidance for that checkpoint:

- Source start/set-caps: arrange access to the CameraService/session-owned, already configured HAL context; validate caps against its confirmed active configuration. The plugin does not independently configure/create/own the instance; camera-list discovery is not an existing HAL API.
- Source create: bounded wait → acquire → validate/copy → checked HAL release → produce GstBuffer. Every HAL call respects caller serialization.
- Source unlock: signal flushing/cancellation in a thread-safe way without a HAL call. A current bounded `wait_frame` must finish; `create` checks flushing before starting wait/acquire and after completed calls, then attempts mandatory checked release/cleanup for any acquired token. Return `GST_FLOW_FLUSHING` with no buffer only when no token remains outstanding. Failed release returns `GST_FLOW_ERROR` with no buffer, reports a fault and retains mapping or pending-cleanup ownership for serialized cleanup. Subsequent creates keep returning flushing until `unlock_stop`, subject to the separate Stop intent.
- Source unlock_stop: clear the GstBaseSrc unlock condition only when allowed by its state transition. The separate session Stop intent still blocks new work during teardown, even if GStreamer invokes `unlock_stop`; only a newly configured session may acquire again.
- Source stop: prevent new work and let the CameraService-owned operation gate observe that all in-flight create/acquire/release and cleanup have finished. The controller completes the pipeline transition to NULL; only then may the service perform state-valid HAL cleanup and release its module reference. HAL stop is skipped in Configured, and a failed stop must not be followed by an invalid close; best-effort destroy still runs after quiescence. The plugin does not decide instance destruction. Use finite HAL waits and a bounded stop policy; never close/destroy HAL concurrently to interrupt wait or invent a HAL cancel API.

**Service process lifetime ≠ pipeline session lifetime.** Service có thể sống trong khi stream stop; CameraService/session owns HAL teardown under the later M4 decision, not source stop. Không gọi `gst_deinit` mỗi lần stop stream nếu process cần chạy tiếp.

## 7. Error handling, bus và event loop

GstBus message dùng cho errors/EOS/state diagnostics, **không** được xem như data plane buffer path. Use one consumption mode per bus: blocking wait or service watch/drain, never competing consumers. `gst_bus_get_pollfd()` returns a borrowed notification FD: never directly read/write/close it; keep its bus alive, poll readiness and drain through GstBus APIs. Drain pending messages without a busy loop or lost wakeup. Message ERROR/EOS/WARNING handling must preserve typed failure/completion and the cleanup rules in §2.

GStreamer error → CameraService state/error → gRPC `GetStatus` (hoặc event notification nếu được thiết kế). Lỗi không chỉ log rồi để RPC luôn trả OK. Khi camera unplug, V4L2 backend báo lỗi, source dừng/flow error, pipeline phát bus error; service nhận, cleanup và có strategy retry/reconnect có giới hạn nếu yêu cầu.

Không luôn cần custom command queue/eventfd: nếu một GLib main context / existing poll loop xử lý được command và bus events an toàn, reuse. Chỉ thêm event loop/queue khi concurrency/lifecycle contract chứng minh cần. Không block main context bằng wait vô hạn.

## 8. Media pipeline và network

Pipeline biểu diễn capture → optional video convert/encode → payload/transport → network sink. Laptop có corresponding receiver/depay/decode/display. **RTP/UDP là candidate**, không được tự động thêm RTSP/WebRTC/browser/cloud. C270 có MJPEG hardware mode nhưng HAL v1 YUYV; encoding YUYV trên BBB có thể tốn CPU. Đánh giá encoder availability (software/hardware), format/caps, bandwidth và memory trước khi freeze transport.

Không phát raw full-resolution frames qua unary gRPC. gRPC chỉ điều khiển, status và cấu hình. Video packet transport và client buffer latency thuộc data plane.

## 9. Tests và debug

Host tests:

1. `gst-inspect-1.0 camstreamsrc`: plugin registration, properties/pad templates/caps.
2. Simulated backend → `camstreamsrc ! fakesink`: output pattern/stride/timestamps, cleanup.
3. Compare known YUY2 frame payload against expected pixels; test caps mismatch.
4. Fail HAL acquire/release, buffer allocation, pipeline negotiation, stop while outstanding.
5. State loop NULL/READY/PAUSED/PLAYING with repeated start/stop.
6. Bus error propagation to service, cancel blocking wait, no deadlock/leak.

BBB integration:

- Real C270 source to local `fakesink`, then video output/receiver.
- Measure actual FPS, dropped frames, latency, CPU/RSS peak, number of copies if instrumented.
- Test USB unplug/replug and downstream slow receiver.
- Compare diagnostic `v4l2src` in **separate runs**, never as parallel owner of same camera.

Evidence tools: `GST_DEBUG` targeted categories, `gst-launch-1.0`, `gst-inspect-1.0`, `GST_DEBUG_DUMP_DOT_DIR` (when useful), bus logs, valgrind/ASan host as supported.

## 10. Primary references

- GStreamer `GstBaseSrc`, `GstPushSrc`: https://gstreamer.freedesktop.org/documentation/base/gstbasesrc.html and https://gstreamer.freedesktop.org/documentation/base/gstpushsrc.html
- GstBuffer wrapping/notify: https://gstreamer.freedesktop.org/documentation/gstreamer/gstbuffer.html
- Plugin allocation: https://gstreamer.freedesktop.org/documentation/plugin-development/advanced/allocation.html
- Element state model: https://gstreamer.freedesktop.org/documentation/additional/design/states.html
- Bus ownership/poll descriptor: https://gstreamer.freedesktop.org/documentation/gstreamer/gstbus.html

