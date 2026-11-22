# M7-B RTP/JPEG network video — owner validation guide

**CODE IMPLEMENTED — NOT BUILT / NOT TESTED.** Every command here is **NOT EXECUTED — OWNER ACTION REQUIRED**.
Codex has not built, tested, cross-built, deployed or run M7-B on BBB. The owner-reported M6 BBB gRPC control pass is
separate from M7 media acceptance. `STAGE7-USB-01` C270 USB/MUSB resets and recoverable V4L2 errors remain open.

## Ownership and media contract

Control stays Ubuntu gRPC client → BBB gRPC server → one CameraService. The service owns the HAL instance/module client
reference. `camstreamsrc` borrows HAL, copies rows into GstBuffer-owned memory and releases the HAL token before
publication. GStreamerController owns the pipeline/bus and quiesces the source before service HAL teardown. No frame
payload goes through gRPC; the `.proto`, adapter and client API are unchanged.

Default mode keeps `camstreamsrc ! fakesink`. Explicit `--media-mode rtp-jpeg` uses:

```text
camstreamsrc ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream
    ! jpegenc quality=85 ! rtpjpegpay pt=26 mtu=1200
    ! udpsink host=<Ubuntu IPv4> port=<UDP port> sync=false async=false
```

The queue drops the oldest raw frame when downstream falls behind. JPEG directly accepts the source's YUY2 caps;
there is no second V4L2 path, BBB `videoconvert` or HAL format change. RTP/JPEG mode rejects active YUYV dimensions
that are not multiples of eight or exceed 2040 before publishing Configured state. Start confirms local pipeline
startup only. UDP send does not establish receiver delivery.

The operator supplies immutable `--media-host <numeric IPv4>` and `--media-port <1..65535>` with network mode and
selects a unicast receiver address. Local mode rejects those options. The code rejects 0/8 and 224/4–255/4;
interface-specific directed broadcasts require operator review. Loopback is accepted for host tests. Non-loopback gRPC
still requires `--allow-insecure-lan`. Media is unauthenticated UDP for a trusted,
isolated lab. No RPC can redirect it.

`local_pipeline_frames` counts local fakesink handoffs in local mode, and complete JPEG buffers at the encoder output
before RTP packetization in network mode. A growing value never proves UDP receipt, decoded frames or playback. The
receiver must measure those separately. Source PTS remains a pipeline-running-time approximation; duration is nominal
from active FPS. Do not subtract unsynchronized BBB and Ubuntu monotonic timestamps to claim one-way latency.

## Buildroot dependency boundary

The defconfig selects GStreamer Good JPEG, RTP and UDP, and disables Good's default AVI, ISOMP4 and WAVPARSE choices.
Good's JPEG option selects a JPEG provider. The generated configuration, actual target plugins, library closure,
incremental image size and ARM runtime behavior remain **NOT VERIFIED**. The older M6 Buildroot output does not prove
the M7 media elements exist. Ubuntu needs `udpsrc`, `rtpjitterbuffer`, `rtpjpegdepay`, `jpegdec`, `videoconvert` and a
video sink; verify the receiver's RTP/JPEG depayloader security patch status before use on an untrusted network
([GStreamer advisory](https://gstreamer.freedesktop.org/security/sa-2026-0057.html)).

## Ubuntu host — NOT EXECUTED — OWNER ACTION REQUIRED

Prerequisites: matching host gRPC/Protobuf development tools and GStreamer core/base/app/Good packages. From the repo:

```sh
cmake -S . -B .local-artifacts/m7/host -DCAMSTREAM_ENABLE_GRPC=ON -DBUILD_TESTING=ON \
  -DCMAKE_C_FLAGS='-Wall -Wextra -Wpedantic -Werror' \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build .local-artifacts/m7/host -j4
ctest --test-dir .local-artifacts/m7/host --output-on-failure
GST_PLUGIN_PATH="$PWD/.local-artifacts/m7/host/plugins/camstreamsrc" gst-inspect-1.0 camstreamsrc
for element in queue jpegenc rtpjpegpay udpsink udpsrc rtpjitterbuffer rtpjpegdepay jpegdec; do
  gst-inspect-1.0 "$element" || exit 1
done
git diff --check
clang-format-14 --dry-run --Werror \
  libs/gstreamer-controller/include/camstream/controller/gstreamer_controller.h \
  libs/gstreamer-controller/src/gstreamer_controller.cpp \
  apps/camera-service/include/camstream/service/camera_service.h \
  apps/camera-service/src/camera_service.cpp apps/grpc-control/src/server_main.cpp \
  tests/m7/network_pipeline_test.cpp tests/m7/server_options_test.cpp
```

The new tests cover local mode, invalid destinations, simulated-backend RTP/JPEG loopback, two cycles, no receiver,
missing encoder, incompatible active caps and injected link/state cleanup. Run the full HAL/M4/M5/M6 regressions too.
For a separate host sanitizer run with a matching gRPC/Protobuf SDK, the owner may use:

```sh
cmake -S . -B .local-artifacts/m7/host-san -DCAMSTREAM_ENABLE_GRPC=ON -DBUILD_TESTING=ON \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build .local-artifacts/m7/host-san -j4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir .local-artifacts/m7/host-san --output-on-failure
```

These commands are **NOT EXECUTED — OWNER ACTION REQUIRED**. `detect_leaks=0` is not LeakSanitizer evidence.

## ARM/Buildroot — NOT EXECUTED — OWNER ACTION REQUIRED

Use the pinned Buildroot source and an isolated M7 output. The owner performs every step:

```sh
RVM_ROOT="$PWD"
BR_SRC="$HOME/TungNHS/camstream-workspace/sources/buildroot-2026.02.3"
BR_OUT="$RVM_ROOT/.local-artifacts/m7/buildroot"
make -C "$BR_SRC" O="$BR_OUT" BR2_EXTERNAL="$RVM_ROOT/br2-external" \
  BR2_DEFCONFIG="$RVM_ROOT/br2-external/configs/rvm_bbb_defconfig" defconfig
make -C "$BR_SRC" O="$BR_OUT" BR2_EXTERNAL="$RVM_ROOT/br2-external" -j4
grep -E '^BR2_PACKAGE_GST1_PLUGINS_GOOD(_JPEG|_PLUGIN_RTP|_PLUGIN_UDP)?=|^BR2_PACKAGE_JPEG=' "$BR_OUT/.config"
find "$BR_OUT/target/usr/lib/gstreamer-1.0" -maxdepth 1 \
  \( -name 'libgstjpeg.so' -o -name 'libgstrtp.so' -o -name 'libgstudp.so' \) -print
cmake -S . -B .local-artifacts/m7/arm -DCMAKE_TOOLCHAIN_FILE="$BR_OUT/host/share/buildroot/toolchainfile.cmake" \
  -DCAMSTREAM_ENABLE_GRPC=ON -DBUILD_TESTING=OFF \
  -DCAMSTREAM_PROTOC_EXECUTABLE="$BR_OUT/host/bin/protoc" \
  -DCAMSTREAM_GRPC_CPP_PLUGIN_EXECUTABLE="$BR_OUT/host/bin/grpc_cpp_plugin" \
  -DCMAKE_C_FLAGS='-Wall -Wextra -Wpedantic -Werror' \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build .local-artifacts/m7/arm -j4
cmake --install .local-artifacts/m7/arm --prefix "$RVM_ROOT/.local-artifacts/m7/arm-install"
file .local-artifacts/m7/arm-install/bin/camstream-grpc-server
readelf -A .local-artifacts/m7/arm-install/bin/camstream-grpc-server
readelf -d .local-artifacts/m7/arm-install/bin/camstream-grpc-server
du -sh "$BR_OUT/target" .local-artifacts/m7/arm-install
```

Inspect ARM ELF/loader, target NEEDED closure, host-path/RPATH leakage, HAL/SPI ABI parity and ext4 free space.
Selecting a Buildroot package does not prove an element factory loads on BBB.

After reviewing the matching image and ARM install tree, the owner may prepare and copy the application tree manually:

```sh
tar -C .local-artifacts/m7/arm-install -czf .local-artifacts/m7/arm-install.tar.gz .
scp .local-artifacts/m7/arm-install.tar.gz root@192.0.2.10:/tmp/
```

These commands are **NOT EXECUTED — OWNER ACTION REQUIRED**. Replace the documentation address; verify the target
rootfs has the matching toolchain/libc and Good plugins before running the copied application.

## BBB and Ubuntu receiver — NOT EXECUTED — OWNER ACTION REQUIRED

Prerequisites: the owner boots a matching M7 image, manually copies the ARM install tree into `/opt/rvm-m7/v1`, checks
library closure, connects C270 and selects the **Video Capture** node, not a UVC metadata node. Do not overwrite
`/usr/lib` with an unrelated bundle. Replace example addresses and paths with verified lab values.

On Ubuntu, start the receiver before StartStream:

```sh
gst-launch-1.0 -v udpsrc port=5004 \
  caps='application/x-rtp,media=(string)video,encoding-name=(string)JPEG,payload=(int)26,clock-rate=(int)90000' \
  ! rtpjitterbuffer latency=100 drop-on-latency=true \
  ! rtpjpegdepay ! jpegdec ! videoconvert ! autovideosink
```

For a separate counted run, stop `gst-launch-1.0` first and use the host M7 receiver diagnostic on the same port in
another Ubuntu terminal. It reports decoded frame count, caps dimensions and first/last PTS for a bounded interval;
it does not measure display latency. Start it before the corresponding gRPC Start and repeat for the restart cycle:

```sh
.local-artifacts/m7/host/tests/m7/camstream-m7-network-test --receive 5004 15
```

On BBB, after manual provisioning on a trusted LAN:

```sh
mkdir -p /opt/rvm-m7/v1
tar -xzf /tmp/arm-install.tar.gz -C /opt/rvm-m7/v1
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --all
export LD_LIBRARY_PATH=/opt/rvm-m7/v1/lib
export GST_PLUGIN_PATH=/opt/rvm-m7/v1/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0
for element in camstreamsrc jpegenc rtpjpegpay udpsink; do gst-inspect-1.0 "$element" || exit 1; done
ldd /opt/rvm-m7/v1/bin/camstream-grpc-server
/opt/rvm-m7/v1/bin/camstream-grpc-server \
  --backend /opt/rvm-m7/v1/lib/camstream/backends/libcamstream-camera-backend-v4l2.so \
  --plugin /opt/rvm-m7/v1/lib/gstreamer-1.0/libgstcamstreamsrc.so \
  --source /dev/videoX --listen 0.0.0.0:50051 --allow-insecure-lan \
  --media-mode rtp-jpeg --media-host 192.0.2.20 --media-port 5004 \
  > /tmp/rvm-m7-server.log 2>&1 &
RVM_SERVER_PID=$!
cat "/proc/$RVM_SERVER_PID/status" | grep -E 'VmRSS|VmPeak|Threads'
cat "/proc/$RVM_SERVER_PID/stat"
cat /proc/net/dev
```

On Ubuntu, control the BBB using the locally built M7 client:

```sh
CLIENT=.local-artifacts/m7/host/apps/grpc-control/camstream-grpc-client
BBB_ADDR='192.0.2.10:50051'
"$CLIENT" --server "$BBB_ADDR" list
"$CLIENT" --server "$BBB_ADDR" status
"$CLIENT" --server "$BBB_ADDR" capabilities
"$CLIENT" --server "$BBB_ADDR" configure 640 480 15 1
"$CLIENT" --server "$BBB_ADDR" start
"$CLIENT" --server "$BBB_ADDR" status
sleep 5
"$CLIENT" --server "$BBB_ADDR" status
"$CLIENT" --server "$BBB_ADDR" stop
"$CLIENT" --server "$BBB_ADDR" configure 640 480 15 1
"$CLIENT" --server "$BBB_ADDR" start
"$CLIENT" --server "$BBB_ADDR" status
"$CLIENT" --server "$BBB_ADDR" stop
```

`192.0.2.20` and `192.0.2.10` are documentation placeholders. Record receiver-decoded frame count, negotiated caps,
PTS monotonicity, frame gaps and display separately from the BBB local JPEG counter. Collect network bytes, queue
behavior, CPU/RSS, server/bus logs and USB/MUSB events. Use synchronized clocks or a visual event for one-way latency.
Sample `/proc/$RVM_SERVER_PID/stat`, `/proc/$RVM_SERVER_PID/status` and `/proc/net/dev` again during each cycle for
CPU ticks, RSS and interface-byte deltas; record the sample interval and interface. On BBB, stop the server and collect logs:

```sh
kill -TERM "$RVM_SERVER_PID"
wait "$RVM_SERVER_PID"
cat /tmp/rvm-m7-server.log
dmesg | grep -Ei 'musb|usb|uvcvideo|video' | tail -n 200
```

No BBB media PASS, sustained FPS, latency, CPU/RSS or USB root-cause claim is made by this source-only checkpoint.
Production transport acceptance needs owner-measured thresholds and logs.
