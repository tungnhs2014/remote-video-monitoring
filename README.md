# Remote Video Monitoring System

An Embedded Linux project designed to capture, process, and stream video
from a USB camera to remote clients.

The system targets the BeagleBone Black and follows a modular architecture
built around a C11 Camera HAL, Linux V4L2, GStreamer, and gRPC.

## 1. System Architecture

The system separates camera acquisition, video processing, and remote control
into independent components.

| Component | Technology | Responsibility |
|---|---|---|
| Camera HAL | C11 | Camera abstraction, backend management, resource ownership |
| V4L2 Backend | C11 | Linux camera device interaction |
| Simulated Backend | C11 | Hardware-independent development and testing |
| GStreamer Source | GStreamer / GObject | Camera HAL integration into the video pipeline |
| Pipeline Controller | C++17 | Pipeline lifecycle and video processing |
| Camera Service | C++17 | Camera operations and application coordination |
| Control Plane | gRPC / Protocol Buffers | Remote camera control |
| Build System | CMake / Buildroot | Host development and Embedded Linux integration |

The video data plane uses GStreamer, while the control plane uses gRPC.

## 2. Target Platform

- Board: BeagleBone Black
- Operating System: Embedded Linux
- Root Filesystem: Buildroot
- Camera: Logitech C270 USB Webcam
- Camera Interface: Video4Linux2 (V4L2)
- Programming Languages: C11 and C++17

## 3. Development Status

Development is organized into incremental milestones.

| Milestone | Status |
|---|---|
| M1 — Project foundation and build system | Completed |
| M2-A — Camera HAL and simulated backend | Completed |
| M2-B — V4L2 Phase-1 camera discovery | Completed |
| M3 — V4L2 configuration and frame capture | Completed; BBB C270 640x480@15 FPS owner-reported PASS |
| M4-A — camstreamsrc copy-first source | Host verified; M4-B BBB functional smoke owner-reported PASS |
| M4-B — session-owner smoke harness | Host verified; owner-reported BBB 640x480@15 FPS, 30 frames × 2 cycles PASS |
| R1 — architecture and coding-style adoption | Local interface/documentation update; BBB after R1 NOT RUN |
| R1-B — HAL Core and V4L2 internal source split | Host/ARM locally verified; BBB 2 × 30-frame C270 functional smoke owner-reported PASS |
| M5 — CameraService and GStreamerController | Host and ARM build locally verified; BBB NOT RUN |
| gRPC remote control | Planned |
| BeagleBone Black hardware validation | M3 and M4-B functional smoke owner-reported PASS; USB/MUSB reliability open |

The V4L2 backend implements YUYV configuration and MMAP streaming through Camera HAL. The M4-A plugin copies frames
into GStreamer-owned buffers before releasing HAL tokens. The owner-reported C270 USB/MUSB reset issue with an
unpowered hub shared with Wi-Fi remains open; its hardware root cause is unconfirmed. Remote control and network
media delivery are not implemented.

## 4. Build and Test

### Requirements

- CMake 3.16 or later
- C11 and C++17 compatible compilers
- Linux development environment
- GStreamer and GLib development packages for `camstreamsrc` and its host tests

### Host Build

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Target Build

ARM cross-compilation is supported through the existing CMake build system.

The saved Buildroot defconfig selects GStreamer for the ARM toolchain. Build the M4-B session-owner
harness with `-DCAMSTREAM_M4_SESSION_SMOKE=ON`. The harness accepts a backend module, plugin module,
source identifier, YUYV mode, frame count and cycle count. An ARM cross-build does not establish
plugin runtime behavior on the board.

## 5. Verification

Current development verification includes:

- Host compilation and regression tests
- Camera HAL ABI compatibility checks
- V4L2 backend regression tests using simulated ioctl responses
- AddressSanitizer and UndefinedBehaviorSanitizer testing
- ARM cross-compilation

The owner reports M3 C270 streaming, M4-B functional smoke and a post-R1-B smoke with two 30-frame cycles at
YUYV 640x480@15 FPS on BeagleBone Black. M5 has not been run on the board. This functional evidence does not resolve
`STAGE7-USB-01` USB/MUSB stability or establish long-run delivered throughput.

## 6. Project Goals

- Maintain a modular and portable camera abstraction.
- Separate video acquisition from application control.
- Support multiple camera backends.
- Follow safe resource ownership and lifecycle management.
- Build maintainable C11 and C++17 components.
- Integrate video streaming and remote control incrementally.
- Validate the complete system on real Embedded Linux hardware.

## 7. Project Status

**Work in progress.**

The repository contains the project foundation, Camera HAL, simulated and V4L2 backends, the M4-A
custom source and the M4-B session-owner smoke harness.

Further functionality will be developed and validated
incrementally on the target hardware.
