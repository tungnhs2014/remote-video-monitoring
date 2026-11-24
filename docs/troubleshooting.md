# Troubleshooting

Diagnose the path in the order it runs: USB hardware → Linux/V4L2 → backend and HAL → GStreamer → gRPC control → UDP network → Ubuntu decoder. Start with the first layer that cannot demonstrate its expected result. Commands below are owner instructions for Ubuntu or a booted BBB; they were not run during this documentation change. Replace `/dev/videoX`, addresses and build paths with observed values. Keep Wi-Fi credentials and private captures out of public issues.

## 1. Power, USB and booted image

If the camera vanishes or frames fail intermittently, check BBB power, USB connection and the kernel log before changing application code:

```sh
dmesg | grep -Ei 'usb|musb|uvcvideo|video'
ls -l /dev/video*
```

USB/MUSB resets are real observations, but a log line does not identify whether the camera, hub, power supply, cable, controller or driver caused them. Preserve timestamps and compare controlled hardware changes one at a time. The backend deliberately discards and requeues a `V4L2_BUF_FLAG_ERROR` frame when possible; a failed requeue is more serious. Do not infer a gRPC defect from a camera reset.

If the BBB seems to run an old image, compare the image built on Ubuntu with the **booted** root filesystem. On BBB, inspect `cat /proc/cmdline`, `mount | grep ' / '`, `cat /etc/os-release`, and `test -x /usr/bin/camstream-grpc-server`. An Ubuntu `target/` listing does not prove the board booted that image. Revisit the [manual microSD procedure](build-and-deployment.md#flash-and-boot-the-microsd-card) only after identifying the exact media and preserving its data.

## 2. V4L2 device and negotiated mode

If `capabilities` or Configure fails, identify the real capture node on BBB:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --all
v4l2-ctl -d /dev/videoX --list-formats-ext
```

Select a **Video Capture** node, not an adjacent UVC metadata node. An advertised YUYV 640×480 at 15 FPS means the driver reports the mode; it does not prove delivery at 15 FPS. [`v4l2_configure()`](../backends/camera/v4l2/src/v4l2_config.c) requests format and interval, reads back active values and validates them. Inspect the service log and client Status for requested/active differences. In RTP/JPEG mode, active dimensions must also meet the controller's RFC 2435 constraints. Choose a mode supported by both the driver and pipeline; do not force a requested value into the active report.

The [V4L2 stream implementation](../backends/camera/v4l2/src/v4l2_stream.c) owns `REQBUFS`, MMAP, queueing, `STREAMON`, bounded `poll`, `DQBUF` and release `QBUF`. A frame error warning can be recovered by requeueing; an invalid buffer, failed `QBUF`, failed cleanup or repeated no-frame timeout needs its specific logged status. Record the first failure and its `errno` context. Avoid simultaneous diagnostic capture through `v4l2-ctl --stream-*` while RVM owns the camera.

## 3. Backend loading, HAL state and tokens

When the server reports a backend load or create failure, verify the **booted** ARM files and loader closure:

```sh
ls -l /usr/lib/camstream/backends/
ldd /usr/bin/camstream-grpc-server
ldd /usr/lib/gstreamer-1.0/libgstcamstreamsrc.so
cat /var/log/remote-video-monitoring.log
```

Look for missing libraries or a wrong backend path. Do not copy an Ubuntu x86_64 `.so` to BBB. Runtime validates the SPI v2 binding at load; a constructor must register callbacks without device I/O. The [HAL Core](../libs/camera-hal/src/camera_core.c) owns public frame-token mappings and removes a mapping only after successful backend release. A release failure is an ownership problem, not just a lost frame: Controller must quiesce the source and retry pending release before Service destroys the camera or unloads the module. Inspect the first HAL status and last-error diagnostic in the server log; a later cleanup warning should not hide the initiating failure.

## 4. GStreamer source and pipeline

If Start fails or Status changes to Faulted, check source and media factory discovery on BBB:

```sh
gst-inspect-1.0 camstreamsrc
gst-inspect-1.0 jpegenc
gst-inspect-1.0 rtpjpegpay
gst-inspect-1.0 udpsink
cat /var/log/remote-video-monitoring.log
```

Missing `camstreamsrc` points to plugin installation/discovery or a missing shared library. A missing JPEG/RTP/UDP factory points to the target GStreamer package set. Compare the booted files with the [Buildroot artifact map](build-and-deployment.md#build-and-inspect-the-bbb-image-on-ubuntu). The source borrows HAL, validates YUYV/YUY2 caps and copies a frame before release. If frame allocation, validation or HAL release fails, it publishes no normal `GstBuffer` and may surface a GStreamer flow/bus error.

[`CameraService::status()`](../apps/camera-service/src/camera_service.cpp) polls the bus while Streaming. An ERROR/EOS there can move Service to Faulted after an earlier successful Start. If Stop or restart fails, check Status and the server log for a quiescence or cleanup error. The safe order is Controller pipeline NULL/quiescence and source unbind, then HAL stop/close/destroy and backend unload. A forced process kill is not evidence of clean resource release.

## 5. gRPC process, listener and generation

If Ubuntu cannot connect, on BBB verify the service config, process, bound address and log:

```sh
grep '^RVM_' /etc/default/remote-video-monitoring
cat /var/run/remote-video-monitoring.pid
ps | grep '[c]amstream-grpc-server'
netstat -lnt | grep ':50051'
cat /var/log/remote-video-monitoring.log
```

The default `RVM_LISTEN=127.0.0.1:50051` is reachable only on BBB. For a trusted lab, configure the actual BBB interface IP and `RVM_ALLOW_INSECURE_LAN=1`, then restart and verify that `netstat` shows that IP. Plaintext control has no authentication or TLS; do not expose it on an untrusted network. On Ubuntu check the route to the verified BBB address and use that same address in `--server`. A running PID alone does not prove the listener is reachable.

The [gRPC adapter](../apps/grpc-control/src/grpc_adapter.cpp) validates logical `camera0`, fields and `expected_generation` for state-changing calls. If a request times out or a reply is lost, query `status` before retrying: the service may have committed the operation. The client obtains the current generation for each mutation, and the server's generation is process-local. `list` exposes a logical mapping; `capabilities` is the device query. A successful RPC Start proves only that the BBB pipeline entered its local streaming state.

## 6. UDP sender and Ubuntu receiver

If Start succeeds but Ubuntu shows no video, first confirm that `RVM_MEDIA_MODE=rtp-jpeg`, `RVM_MEDIA_HOST` is the Ubuntu machine's current numeric IPv4 address, and `RVM_MEDIA_PORT` matches the receiver's port. Check the BBB log, network route/firewall and the Ubuntu receiver's `gst-launch-1.0 -v` caps/errors. Start the receiver before StartStream. The [receiver command](getting-started.md#3-start-the-ubuntu-receiver) expects JPEG RTP payload 26 at a 90000 Hz clock. UDP has no acknowledgement; sender success cannot establish packet arrival or successful decode.

Keep three measurements distinct: requested FPS in Configure, driver-confirmed active FPS, and **decoded receiver FPS** over a measured interval. BBB `local_pipeline_frames` counts before RTP transmission. For numeric receiver evidence, use the [single-port test receiver](getting-started.md#3-start-the-ubuntu-receiver) after stopping `gst-launch-1.0`; record decoded count, interval, caps and PTS for each cycle. Receiver PTS and nominal duration do not prove camera exposure time or network jitter. If the receiver sees packets but cannot decode, inspect caps and JPEG/RTP elements before changing HAL code.

## Host regressions versus target evidence

On Ubuntu, the owner can run the documented [host build and CTest](getting-started.md#1-build-the-ubuntu-client), and inspect the host plugin with `GST_PLUGIN_PATH="$PWD/.local-artifacts/host/plugins/camstreamsrc" gst-inspect-1.0 camstreamsrc`. These checks exercise host HAL, fake V4L2, source, service and network code according to configured dependencies. Host PASS does not imply ARM build PASS. An ARM ELF check does not imply BBB runtime PASS. Only the booted BBB and Ubuntu receiver can establish the target media path. The [M7 evidence record](verification/m7-verification-report.md) states what the owner previously reported and what remains unmeasured.
