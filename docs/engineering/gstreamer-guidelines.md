# GStreamer source and pipeline guidance

**Scope:** The C/GObject `camstreamsrc` and C++17 GStreamerController. Current behavior is in [component design](../component-design.md) and [runtime lifecycle](../runtime-lifecycle.md). The [system contracts](../architecture/system-contracts.md) own HAL lifetime and ABI rules. `camstreamsrc` borrows a CameraService-owned HAL instance; GStreamer reference counting does not own that session.

## Object and reference model

| Object | Role | Ownership question |
|---|---|---|
| `GstElement` and `GstPad` | Processing element and data connection | Did a bin take the element's reference? Is the pad borrowed or full? |
| `GstCaps` | Negotiated media format | Who unrefs the caps after negotiation? |
| `GstBuffer` and `GstMemory` | Ref-counted payload and metadata | Is memory a HAL-independent copy or borrowed external storage? |
| `GstBus` and `GstMessage` | Error, EOS and state notifications | Which consumer drains and unrefs each message? |
| `GstClock` | Pipeline time base | How does PTS relate to capture and running time? |

Inspect each API's transfer annotation; do not assume all `gst_*_new` or bin-add operations have the same reference behavior. Pair `gst_buffer_map` with `gst_buffer_unmap` and never use mapped data afterward. Free owned `GError`, debug strings and messages on every path. Linking elements succeeds before caps negotiation is proven. `GST_STATE_CHANGE_ASYNC` is not proof that PLAYING has completed.

## Source contract and caps

`camstreamsrc` subclasses `GstPushSrc`. It makes bounded HAL wait/acquire calls, validates token/plane/stride/size, copies YUYV rows into an independent YUY2 `GstBuffer`, then successfully releases the HAL token before output. It never opens `/dev/videoX`, calls V4L2 `ioctl`, owns the HAL instance/module reference, or manages gRPC sockets. The controller binds it to a configured session and owns pipeline and bus state.

V4L2 requested mode, driver-confirmed active mode and GStreamer caps are distinct. Advertise the active YUYV data as GStreamer YUY2; do not claim MJPEG input because the camera advertises it if HAL v1 only supplies YUYV. Validate stride and padded rows instead of assuming tightly packed storage. Active FPS determines nominal buffer duration; negotiated caps do not measure delivered FPS. Source PTS uses pipeline running time near acquisition as a capture-time approximation, not an unverified absolute HAL clock conversion. Mark first frames and sequence gaps with `GST_BUFFER_FLAG_DISCONT`.

<a id="buffer-lifetimes"></a>

## Buffer lifetimes

**Approved copy-first path:** acquire HAL frame → validate/copy into GStreamer-owned storage → checked HAL release → publish. Downstream may keep the copy after the HAL buffer is requeued. Allocation or validation failure still requires a checked release attempt. If release fails, do not publish normal success; retain token or pending cleanup ownership for a serialized retry. A final GstBuffer unref frees only the copy.

Wrapping HAL memory is a separate, unimplemented design. It would require a lease keeping the camera, module and mapping alive until the last downstream memory reference, plus serialized HAL release from an unknown finalizer thread. The HAL has no camera retain/unref API. Calling `gst_buffer_new_wrapped_full` alone does not make an end-to-end zero-copy pipeline, and DMA-BUF support would require its own device/caps/allocator evidence.

## Start, stop and bus errors

GStreamer transitions through NULL, READY, PAUSED and PLAYING. Live-source NO_PREROLL/ASYNC behavior and `GstBaseSrc` `unlock`/`unlock_stop` must be handled according to the class contract. A blocked `create` uses a finite HAL wait and checks flushing before new work and after completed calls. `unlock` requests cancellation without concurrently closing HAL. A separate Stop intent prevents admission even if GStreamer later calls `unlock_stop`. Any acquired token is released or kept for explicit cleanup before quiescence completes.

Controller Stop requests source stop, moves the pipeline to NULL to trigger flushing, waits for NULL/quiescence, retries pending frame release and unbinds. Only afterward may CameraService stop/close/destroy HAL and unload the backend. A service process can outlive many pipeline sessions; do not call `gst_deinit` per Stop. If bounded quiescence fails, retain dependent resources and report failure instead of racing teardown.

The current bus sync handler observes ERROR/EOS and returns `GST_BUS_PASS`, leaving each message queued; `gst_bus_pop()` is the consumer that drains and unreferences queued messages. Do not add a competing bus consumer. A `gst_bus_get_pollfd()` descriptor is borrowed: never read, write or close it directly. Poll readiness and drain messages through GstBus APIs while keeping the bus alive. ERROR/EOS must reach service state/results, not merely the log. Bus messages are not frame payloads. Avoid an extra command queue or executor until a measured concurrency requirement justifies it.

## Network mode and evidence

The implemented network pipeline uses one two-buffer downstream-leaky queue, `jpegenc`, `rtpjpegpay` and `udpsink`; the Ubuntu receiver depayloads and decodes separately. Control remains gRPC; no frame payload enters protobuf. Start/PLAYING and BBB `local_pipeline_frames` do not prove UDP receipt, decoded frames or display. Measure those at the receiver. Keep `v4l2src` diagnostic runs separate from RVM's ownership of the same camera.

Host verification can inspect the plugin with `gst-inspect-1.0` and exercise simulated frames, caps/stride/PTS, allocation/acquire/release faults, flushing, repeated sessions and bus errors. BBB checks separately cover real camera behavior, delivered frame rate, receiver loss/jitter, CPU/RSS and USB events. Neither category is implied by this guidance.

Official API references: [GstBaseSrc](https://gstreamer.freedesktop.org/documentation/base/gstbasesrc.html), [GstPushSrc](https://gstreamer.freedesktop.org/documentation/base/gstpushsrc.html), [GstBuffer](https://gstreamer.freedesktop.org/documentation/gstreamer/gstbuffer.html), [states](https://gstreamer.freedesktop.org/documentation/additional/design/states.html), and [GstBus](https://gstreamer.freedesktop.org/documentation/gstreamer/gstbus.html).
