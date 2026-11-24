# Build and deployment

This guide describes the current BBB Buildroot firmware path and the Ubuntu client/receiver. Commands are for the owner to run and were **not executed for this documentation refactor**. The owner previously reported a successful R3 firmware boot and M7 control/video check; see the [evidence record](verification/m7-verification-report.md) for its limits.

The workflow has four distinct outcomes: build the Ubuntu client for development, cross-build the BBB runtime in Buildroot, inspect the generated ARM image, and only then manually flash and boot a verified microSD card. Run the [first-session guide](getting-started.md) after configuring the service on the booted board. Keep the Buildroot output directory and image identity together when recording evidence.

## Build model

The repository's [top-level CMake file](../CMakeLists.txt) builds C11 HAL/backend targets, a C/GObject plugin, and C++17 controller/service/gRPC targets. `CAMSTREAM_ENABLE_GRPC` defaults to OFF for the host build; set it ON to build the server and client. `BUILD_TESTING` controls regression tests. See [getting started](getting-started.md#1-build-the-ubuntu-client) for a host build and CTest example.

The [Buildroot external tree](../br2-external/) selects the BBB toolchain, kernel, GStreamer, gRPC and package dependencies. Its [application package](../br2-external/package/remote-video-monitoring/remote-video-monitoring.mk) builds the current local checkout with the Buildroot toolchain and `BUILD_TESTING=OFF`. The `Runtime` install component includes the server, HAL shared library, V4L2 backend and `camstreamsrc`. The package installs its config and SysV script. Simulated backend, tests, diagnostic harnesses and Ubuntu client are outside the firmware runtime component.

`SITE_METHOD=local` copies the current working tree, including uncommitted changes. This is useful for development but does **not** produce a release from an immutable source revision. Review `target/` as packaging input, `host/` as cross tools and sysroot, and `images/sdcard.img` as the bootable artifact. A successful build or a populated `target/` does not prove what a BBB has booted. The package excludes private/local outputs from its source copy and currently sets `REDISTRIBUTE=NO`; a public release needs an approved source pin, license and notices review.

| Location | What it is | What it proves |
|---|---|---|
| `.local-artifacts/host/` | Ubuntu CMake output, client and host tests | Host compilation only when actually run |
| `BR_OUT/host/` | Buildroot cross tools and target sysroot | Toolchain/dependency inputs, not target runtime |
| `BR_OUT/target/` | Root filesystem staging tree | Files selected for packaging, not BBB boot |
| `BR_OUT/images/sdcard.img` | Flashable boot/root image | Image creation, not boot or camera operation |
| BBB `/usr/` and `/etc/` | Booted system files | Installed runtime on that board when inspected |

The checked-in [defconfig](../br2-external/configs/rvm_bbb_defconfig) selects the BBB architecture and image inputs; the [package definition](../br2-external/package/remote-video-monitoring/remote-video-monitoring.mk) sets CMake options and target dependencies. Check the effective Buildroot `.config` after `defconfig`: an inherited output or a different Buildroot source can change what is actually built. The source path shown below is an example from prior local inspection, not an automatic download or a universal version pin.

## Build and inspect the BBB image on Ubuntu

First complete [checkout and host preparation](getting-started.md#prepare-the-checkouts-and-ubuntu-host). The inspected local Buildroot source has tag 2026.02.3 at commit `679b9ead7620bbf193620d1ebf56f53c1764d37a`; verify your own checkout and effective `.config`. Buildroot also needs its documented Ubuntu host tools, available disk space and target downloads. The paths below match that preparation recipe; change them if your checkout is elsewhere:

```sh
RVM_ROOT="$HOME/TungNHS/remote-video-monitoring"
BR_SRC="$HOME/TungNHS/camstream-workspace/sources/buildroot-2026.02.3"
BR_OUT="$RVM_ROOT/.local-artifacts/buildroot-bbb-$(date +%Y%m%d-%H%M%S)"
cd "$RVM_ROOT"
"$BR_SRC/utils/check-package" -b \
  br2-external/package/remote-video-monitoring/Config.in \
  br2-external/package/remote-video-monitoring/remote-video-monitoring.mk \
  br2-external/package/remote-video-monitoring/S90remote-video-monitoring
make -C "$BR_SRC" O="$BR_OUT" BR2_EXTERNAL="$RVM_ROOT/br2-external" \
  BR2_DEFCONFIG="$RVM_ROOT/br2-external/configs/rvm_bbb_defconfig" defconfig
grep -E '^BR2_PACKAGE_(REMOTE_VIDEO_MONITORING|GRPC|PROTOBUF|GSTREAMER1|GST1_PLUGINS_BASE|GST1_PLUGINS_GOOD|GST1_PLUGINS_GOOD_JPEG|GST1_PLUGINS_GOOD_PLUGIN_RTP|GST1_PLUGINS_GOOD_PLUGIN_UDP)=' "$BR_OUT/.config"
make -C "$BR_SRC" O="$BR_OUT" BR2_EXTERNAL="$RVM_ROOT/br2-external" -j4
ls -lh "$BR_OUT/images/rootfs.ext4" "$BR_OUT/images/sdcard.img"
printf 'Buildroot output: %s\n' "$BR_OUT"
```

The timestamped output path is intended to be unused; if it already exists, choose another empty path before configuring. Record the printed `BR_OUT` for later artifact and flashing checks. Building only the application package can update `target/` without regenerating `sdcard.img`; a full Buildroot run is required for an image. The [genimage config selected by the defconfig](../br2-external/configs/rvm_bbb_defconfig) produces a FAT boot partition and ext4 root filesystem.

Inspect the target runtime before flashing:

```sh
test -x "$BR_OUT/target/usr/bin/camstream-grpc-server"
test -f "$BR_OUT/target/usr/lib/libcamstream-camera-hal.so"
test -f "$BR_OUT/target/usr/lib/camstream/backends/libcamstream-camera-backend-v4l2.so"
test -f "$BR_OUT/target/usr/lib/gstreamer-1.0/libgstcamstreamsrc.so"
test -L "$BR_OUT/target/usr/lib/libgstcamstreamsrc.so"
test -x "$BR_OUT/target/etc/init.d/S90remote-video-monitoring"
file "$BR_OUT/target/usr/bin/camstream-grpc-server"
readelf -A "$BR_OUT/target/usr/bin/camstream-grpc-server"
readelf -d "$BR_OUT/target/usr/bin/camstream-grpc-server" | grep -E 'NEEDED|RPATH|RUNPATH'
```

Check ARM architecture/interpreter, every `NEEDED` library and symlink in the final image, and absence of host paths in RPATH/RUNPATH. The plugin belongs in `/usr/lib/gstreamer-1.0`; the package also creates `/usr/lib/libgstcamstreamsrc.so` for the server's direct loader dependency. Verify target `jpegenc`, `rtpjpegpay` and `udpsink` factories on the booted BBB; selecting Buildroot packages alone does not prove they load.

For an application-only source edit in this local-source development package, the owner may use Buildroot's `remote-video-monitoring-rebuild` target in the same `BR_OUT`, then regenerate the image with a full Buildroot run. Inspect the resulting timestamps and content before flashing. This incremental path is for development; it does not establish clean release reproducibility or replace the full image check.

## Flash and boot the microSD card

**Owner-only destructive procedure on Ubuntu.** Use the exact `BR_OUT` printed by the image build, in the same shell or after setting it explicitly in a new shell. First confirm the image and compare `lsblk` before and after inserting the microSD card. Record its model, size and **whole-disk** path. Back up any data on that card. Never copy an example `/dev/sdX` path into `dd`.

```sh
ls -lh "$BR_OUT/images/sdcard.img"
sha256sum "$BR_OUT/images/sdcard.img"
lsblk -o NAME,PATH,SIZE,TYPE,TRAN,MODEL,MOUNTPOINTS
findmnt -no SOURCE /
findmnt -no SOURCE /boot
findmnt -no SOURCE /boot/efi
swapon --show=NAME
```

The `/boot` and `/boot/efi` commands may report no separate mount. Trace the root, boot and active swap devices through the full `lsblk` tree, including any LUKS, LVM or MD parents. **Do not continue** if the candidate disk contains any of them or if its ancestry is unclear. Select the microSD **manually**; no script can establish that an arbitrary disk is safe to erase:

```bash
read -r -p 'Confirmed whole microSD device (absolute /dev/... path): ' SD_DEVICE
test -b "$SD_DEVICE"
test "$(lsblk -dn -o TYPE "$SD_DEVICE")" = disk
lsblk -o NAME,PATH,SIZE,TYPE,TRAN,MODEL,MOUNTPOINTS "$SD_DEVICE"
```

Stop if either `test` fails. Compare the displayed model, size, partition tree and device path with the before/after insertion observations. Unmount **each mounted filesystem on that confirmed card**, one at a time. Repeat the block below for each mounted device, entering its actual full path:

```bash
read -r -p 'Confirmed mounted device to unmount (absolute /dev/... path): ' PARTITION
if lsblk -nr -o PATH "$SD_DEVICE" | grep -Fxq -- "$PARTITION"; then
    sudo umount "$PARTITION"
else
    echo 'Device is not under the selected microSD disk; nothing unmounted' >&2
fi
```

Review the selected disk again. All its partitions must have empty `MOUNTPOINTS` and none may be active swap or an ancestor of root/boot. If a mount or swap is still present, or any identity check is uncertain, stop:

```sh
lsblk -o NAME,PATH,SIZE,TYPE,TRAN,MODEL,MOUNTPOINTS "$SD_DEVICE"
swapon --show=NAME
```

Only after those manual checks, enter the exact confirmation below. `dd` erases **all data** on the selected whole disk. The owner must execute this block; it is not part of an automated build or deployment:

```bash
read -r -p "Type ERASE $SD_DEVICE to write the image: " CONFIRM
if [ "$CONFIRM" = "ERASE $SD_DEVICE" ]; then
    if sudo dd if="$BR_OUT/images/sdcard.img" of="$SD_DEVICE" bs=4M status=progress conv=fsync; then
        sync
        lsblk -o NAME,PATH,SIZE,TYPE,TRAN,MODEL,MOUNTPOINTS "$SD_DEVICE"
    else
        echo 'Image write failed; do not boot this card' >&2
    fi
else
    echo 'Cancelled; image was not written'
fi
```

After safely removing the card, boot the BBB from it using the board's established hardware procedure. On the BBB, confirm the running root filesystem and installed files. An Ubuntu `target/` listing is not boot evidence:

```sh
cat /etc/os-release
cat /proc/cmdline
cat /proc/partitions
mount | grep ' / '
test -x /usr/bin/camstream-grpc-server
test -x /etc/init.d/S90remote-video-monitoring
```

## Configure the BBB service

The [init script](../br2-external/package/remote-video-monitoring/S90remote-video-monitoring) reads `/etc/default/remote-video-monitoring`. Its checked-in default is `RVM_LISTEN=127.0.0.1:50051` and `RVM_MEDIA_MODE=local`. It runs after the existing Wi-Fi init script, but startup order does not prove DHCP or receiver readiness. On the booted BBB, find the C270 **Video Capture** node and actual trusted-lab interface IP, then back up and edit the config:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --all  # Replace X with the confirmed Video Capture node.
ip -4 -o addr show dev wlan0   # Or the actual trusted-lab interface.
cp -p /etc/default/remote-video-monitoring /etc/default/remote-video-monitoring.bak
vi /etc/default/remote-video-monitoring
```

Keep existing variables and set real values. The following addresses and `/dev/videoX` are **documentation placeholders**. Bind a specific BBB interface address instead of all interfaces. Enable non-loopback plaintext gRPC only on an isolated, trusted lab network; this system has no TLS/auth for other networks.

```text
RVM_ENABLED=1
RVM_SOURCE=/dev/videoX
RVM_LISTEN=192.0.2.10:50051
RVM_ALLOW_INSECURE_LAN=1
RVM_MEDIA_MODE=rtp-jpeg
RVM_MEDIA_HOST=192.0.2.20
RVM_MEDIA_PORT=5004
```

Save the file, restart, and verify the process, exact local listen address, log, plugin factories and shared-library closure:

```sh
grep '^RVM_' /etc/default/remote-video-monitoring
/etc/init.d/S90remote-video-monitoring restart
cat /var/run/remote-video-monitoring.pid
ps | grep '[c]amstream-grpc-server'
netstat -lnt | grep ':50051'
cat /var/log/remote-video-monitoring.log
gst-inspect-1.0 camstreamsrc
gst-inspect-1.0 jpegenc
gst-inspect-1.0 rtpjpegpay
gst-inspect-1.0 udpsink
ldd /usr/bin/camstream-grpc-server
ldd /usr/lib/gstreamer-1.0/libgstcamstreamsrc.so
```

`netstat` must show the selected BBB interface IP in the local address column. If settings are wrong, edit and restart again; the `.bak` file allows recovery. Start the Ubuntu receiver before issuing StartStream. The [two-cycle client procedure](getting-started.md#control-two-sessions-from-ubuntu) checks control and playback separately.

## Manual lab deployment versus firmware

An earlier M7 lab workflow copied an ARM application bundle to `/opt` or `/tmp` for manual testing. It required a matching target libc and GStreamer dependency set and did not package the complete runtime image. The current R3 Buildroot package installs the application and dependencies into the firmware and is the path documented above. Neither path establishes release reproducibility without an immutable source revision and verified dependency/license closure.
