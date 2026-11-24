# Getting started

This is a first-run path for a BeagleBone Black (BBB) camera session controlled from Ubuntu. The commands are owner instructions checked against the repository; this documentation task did not build an image or operate hardware. Read [system architecture](system-architecture.md) first if the separate control and video paths are unfamiliar.

## Before you begin

You should be comfortable with a Linux terminal, network interface addresses and the difference between a file path and a device node. The [applied concepts guide](applied-embedded-linux.md) explains V4L2, HAL ownership, GStreamer and Buildroot as they appear in this system. The host and BBB are different machines: the host compiler produces Ubuntu binaries; Buildroot produces ARM firmware.

Have an Ubuntu development computer, a BBB with reliable power and network access, a microSD card whose contents may be erased, and a USB UVC camera. The owner previously checked a Logitech C270 at configured YUYV 640×480, 15 FPS; this does not guarantee other cameras or sustained delivery. Use an isolated, trusted lab network for the current plaintext gRPC and unauthenticated UDP setup.

## Prepare the checkouts and Ubuntu host

The checked-in [BBB defconfig](../br2-external/configs/rvm_bbb_defconfig) and [package](../br2-external/package/remote-video-monitoring/remote-video-monitoring.mk) live in `br2-external/`. This checkout was inspected on Ubuntu 22.04.5 LTS with Buildroot tag `2026.02.3` at commit `679b9ead7620bbf193620d1ebf56f53c1764d37a`. That is a verified **source input**, not proof that a new host can build or boot the firmware. The following is an owner-run preparation recipe for a fresh checkout **after these documentation changes are published**:

```sh
mkdir -p "$HOME/TungNHS/camstream-workspace/sources"
git clone https://github.com/tungnhs2014/remote-video-monitoring.git "$HOME/TungNHS/remote-video-monitoring"
git clone https://gitlab.com/buildroot.org/buildroot.git \
  "$HOME/TungNHS/camstream-workspace/sources/buildroot-2026.02.3"
BR_SRC="$HOME/TungNHS/camstream-workspace/sources/buildroot-2026.02.3"
git -C "$BR_SRC" checkout --detach 2026.02.3
test "$(git -C "$BR_SRC" rev-parse HEAD)" = 679b9ead7620bbf193620d1ebf56f53c1764d37a
grep '^export BR2_VERSION := 2026.02.3$' "$BR_SRC/Makefile"
cd "$HOME/TungNHS/remote-video-monitoring"
```

If RVM is already checked out, keep it and verify `git remote -v` and `git status --short` instead of cloning over it. If the Buildroot tag or commit differs, stop and inspect the source; do not silently use a newer release with this defconfig. The [official Buildroot tag](https://gitlab.com/buildroot.org/buildroot/-/tags/2026.02.3) identifies the expected source. `SITE_METHOD=local` later copies the **current RVM working tree**, so a fresh clone of an older published revision will not contain uncommitted documentation or code changes.

Ubuntu development needs a C11/C++17 compiler, CMake 3.16+, `pkg-config`, GStreamer/GLib headers, and matching Protobuf/gRPC tools and libraries. Buildroot additionally needs the [mandatory host utilities listed in its manual](https://buildroot.org/downloads/manual/manual.html), source downloads and substantial free disk space. On Ubuntu 22.04, these package names were checked against the local package index; **installation was not run as part of this documentation task**. A newcomer can review the names with `apt-cache policy`, then install the missing packages on their own host:

```sh
sudo apt update
sudo apt install build-essential binutils diffutils findutils gawk sed \
  cmake pkg-config git ca-certificates \
  libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad bc bzip2 cpio file gzip patch perl rsync \
  tar unzip wget xz-utils python3
```

Package names vary on other Ubuntu releases. These are development-host tools; Buildroot uses its own cross toolchain and target packages. Check the tools and libraries before configuring CMake:

```sh
cmake --version
cc --version
c++ --version
pkg-config --modversion gstreamer-1.0 gstreamer-base-1.0 glib-2.0
pkg-config --modversion grpc++ protobuf
protoc --version
command -v grpc_cpp_plugin
test -x /usr/bin/file
gst-inspect-1.0 udpsrc
gst-inspect-1.0 rtpjpegdepay
gst-inspect-1.0 jpegdec
```

A missing tool, `pkg-config` module or GStreamer factory means host setup is incomplete. These checks do not validate the BBB image. Verify free disk space and network access for Buildroot downloads before its first build; the [build guide](build-and-deployment.md#build-and-inspect-the-bbb-image-on-ubuntu) explains the output directories and expected `rootfs.ext4`/`sdcard.img` files.

## 1. Build the Ubuntu client

From the RVM repository root on Ubuntu:

```sh
cmake -S . -B .local-artifacts/host -DCAMSTREAM_ENABLE_GRPC=ON -DBUILD_TESTING=ON
cmake --build .local-artifacts/host --parallel
ctest --test-dir .local-artifacts/host --output-on-failure
test -x .local-artifacts/host/apps/grpc-control/camstream-grpc-client
```

The executable is `.local-artifacts/host/apps/grpc-control/camstream-grpc-client`. Host tests use simulated or fake I/O and do not prove ARM build or BBB camera behavior. If CMake cannot find gRPC/Protobuf, compare its error with the installed host development files; do not substitute BBB libraries. The [engineering build guide](build-and-deployment.md) describes cross-build and artifact inspection.

## 2. Prepare and boot the BBB image

Follow [Buildroot image creation and inspection](build-and-deployment.md#build-and-inspect-the-bbb-image-on-ubuntu), then the [guarded microSD procedure](build-and-deployment.md#flash-and-boot-the-microsd-card). The owner confirms the whole disk before `dd`; this page intentionally does not duplicate destructive commands.

For BBB microSD boot, power the board down, insert the prepared card, hold the **BOOT** switch while applying power, then release it after power-up starts. The [official BBB boot-mode description](https://docs.beagleboard.org/boards/beaglebone/black/ch05.html#boot-modes) explains why this selects microSD over the normal eMMC path; pressing RESET alone does not resample the boot pins. Do **not** run an eMMC flasher as part of this procedure. On the booted BBB, use the checks in the build guide to confirm the root filesystem, server and init script are from the intended image. A populated Ubuntu `target/` directory alone does not establish what the board booted.

Identify the camera's **Video Capture** node with `v4l2-ctl --list-devices` and `v4l2-ctl -d /dev/videoX --all`, replacing `X` with the confirmed node. A UVC metadata node is not a capture node. Then follow [BBB service configuration](build-and-deployment.md#configure-the-bbb-service): back up `/etc/default/remote-video-monitoring`, set the real source node, specific BBB interface address, Ubuntu IPv4 destination and UDP port, restart the init script, and verify its process, listener and log. The default is loopback control plus local `fakesink`; network video requires an explicit trusted-lab configuration.

## 3. Start the Ubuntu receiver

On Ubuntu, use the same UDP port configured as `RVM_MEDIA_PORT` on BBB. Start this before requesting StartStream:

```sh
gst-launch-1.0 -v udpsrc port=5004 \
  caps='application/x-rtp,media=(string)video,encoding-name=(string)JPEG,payload=(int)26,clock-rate=(int)90000' \
  ! rtpjitterbuffer latency=100 drop-on-latency=true \
  ! rtpjpegdepay ! jpegdec ! videoconvert ! autovideosink
```

`-v` prints negotiated caps. A visible image and decoder activity are receiver observations; a successful BBB Start is only a sender observation. For a numeric decoded-frame check, stop `gst-launch-1.0` so only one process owns UDP port 5004, then run the host test receiver immediately before **each** Start:

```sh
.local-artifacts/host/tests/m7/camstream-m7-network-test --receive 5004 15
```

It reports decoded frames, interval, dimensions and first/last PTS. It is available only with `BUILD_TESTING=ON`. Repeat for the second cycle; do not infer decoded FPS from BBB counters.

## Control two sessions from Ubuntu

In another terminal at the repository root, replace the example BBB address with the exact interface address configured in `RVM_LISTEN`:

```sh
CLIENT=.local-artifacts/host/apps/grpc-control/camstream-grpc-client
BBB_ADDR=192.0.2.10:50051  # Documentation address; replace it.
"$CLIENT" --server "$BBB_ADDR" list
"$CLIENT" --server "$BBB_ADDR" capabilities
"$CLIENT" --server "$BBB_ADDR" configure 640 480 15 1
"$CLIENT" --server "$BBB_ADDR" start
sleep 10
"$CLIENT" --server "$BBB_ADDR" status
"$CLIENT" --server "$BBB_ADDR" stop
"$CLIENT" --server "$BBB_ADDR" status
"$CLIENT" --server "$BBB_ADDR" configure 640 480 15 1
"$CLIENT" --server "$BBB_ADDR" start
sleep 10
"$CLIENT" --server "$BBB_ADDR" status
"$CLIENT" --server "$BBB_ADDR" stop
"$CLIENT" --server "$BBB_ADDR" status
```

The client obtains the current generation before each mutation. `list` reports the logical `camera0` mapping; `capabilities` actually queries the backend. Confirm Configured after Configure, Streaming after Start and Idle after each Stop. During each approximate ten-second observation, check Ubuntu caps/video or decoded frames separately. BBB `local_pipeline_frames` is counted before RTP transmission and cannot measure receiver FPS. If a mutating RPC times out, query Status before retrying because the operation may have committed.

If the service or receiver fails, follow [troubleshooting](troubleshooting.md) in hardware-to-client order. For deeper verification, see the [M7 evidence record](verification/m7-verification-report.md); it distinguishes owner-reported checks from unmeasured stability and performance.
