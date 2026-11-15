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
| V4L2 configuration and frame capture | Planned |
| GStreamer integration | Planned |
| gRPC remote control | Planned |
| BeagleBone Black hardware validation | Pending |

The current V4L2 implementation supports device capability inspection
and enumeration of supported discrete YUYV configurations.

Camera configuration, frame acquisition, video streaming, and remote
control are not yet implemented.

## 4. Build and Test

### Requirements

- CMake 3.16 or later
- C11 and C++17 compatible compilers
- Linux development environment
- GStreamer development packages for the optional GStreamer smoke test

### Host Build

```bash
cmake -S . -B build -DCAMSTREAM_M1_GSTREAMER_SMOKE=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Target Build

ARM cross-compilation is supported through the existing CMake build system.

Target toolchain selection and reproducible Buildroot integration will
be finalized as development progresses.

## 5. Verification

Current development verification includes:

- Host compilation and regression tests
- Camera HAL ABI compatibility checks
- V4L2 backend regression tests using simulated ioctl responses
- AddressSanitizer and UndefinedBehaviorSanitizer testing
- ARM cross-compilation

Real-camera execution on the BeagleBone Black remains pending.

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

The repository currently contains the project foundation,
Camera HAL, simulated backend, and initial V4L2 implementation.

Further functionality will be developed and validated
incrementally on the target hardware.