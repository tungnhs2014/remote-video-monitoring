# M7 functional evidence and open checks

This record separates **owner-reported results** from source inspection and unmeasured behavior. The owner reported an R3 Buildroot image booting on a BBB, with gRPC control and RTP/JPEG decode/display on Ubuntu. The R4 documentation work did not rebuild, reflash or run BBB tests. At the original R4 review, Git HEAD was `80f861b` while R2/R3 work was still present in the working tree, so that commit alone was **not** the tested firmware identity. No complete image manifest or log set was supplied for an independent replay of the numbers below.

## Reported functional evidence

| Area | Reported result | What it establishes |
|---|---|---|
| Host R2-A CTest | 16/16 PASS, owner-reported | That checkpoint's host tests; not this documentation tree or the BBB image |
| R3 firmware | Build and boot PASS, owner-reported | BBB ran an image with server, libraries and media plugins installed; not immutable-source reproducibility |
| Camera mode | Logitech C270 configured at YUYV 640×480, 15 FPS | Requested/active configuration, not decoded receiver FPS |
| gRPC lifecycle | Configure → Start → Stop → Configure → Start → Stop, final IDLE, generation 0→6 | Two control cycles on one server process |
| Ubuntu receiver | RTP/JPEG video decoded and displayed | Functional media delivery on the reported lab setup |
| BBB counters | 142 and 109 frames over approximately 10 seconds per cycle | Local JPEG-buffer observation before RTP, not remote decoded frames |
| Runtime packaging | Owner reported no temporary `/tmp` library staging required | Application and dependencies were available from the firmware image |

The BBB counter must not be divided by “about 10 seconds” and presented as measured receiver FPS. A delivered-rate claim needs decoded-frame counts over a measured interval on Ubuntu. Advertised V4L2 intervals, active GStreamer caps and local sender counters answer different questions.

## Implemented design confirmed by source inspection

One CameraService owns the HAL camera and backend module reference. The C11 HAL/Core/runtime dispatches through backend SPI v2 to the V4L2 implementation; `camstreamsrc` borrows HAL, copies into GStreamer memory and releases each frame token before publication. GStreamerController owns the pipeline and bus. The gRPC control plane carries no video; network mode uses a bounded queue, JPEG/RTP payloader and UDP sink. The R3 Buildroot package provides a SysV service and target runtime dependencies. These are **source/configuration facts**, not fresh runtime tests.

## Limits and release prerequisites

- C270 USB resets and recoverable V4L2 frame warnings were observed; their relationship and root cause are unconfirmed.
- Sustained operation, decoded receiver FPS, packet loss/jitter, latency, CPU/RSS and controlled USB recovery remain **NOT VERIFIED** by the reported two short cycles.
- The server uses plaintext gRPC and RTP/UDP without authentication; non-loopback control requires explicit opt-in and belongs only on an isolated trusted lab network.
- The Buildroot package currently uses the local working tree as source and sets `REDISTRIBUTE=NO`. A public release needs an approved immutable revision/archive, complete license and third-party notices review, and source/history/private-reference inspection.
- Host build and test, ARM ELF/dependency closure, booted-image identity and target runtime should be recorded again against the exact revision and image selected for release. None were run by this documentation task.
- Raspberry Pi integration is a future task; the current Buildroot firmware and reported hardware result target BBB.

For reproduction, follow [getting started](../getting-started.md) and [build and deployment](../build-and-deployment.md). For ABI and ownership rules, use the [system contracts](../architecture/system-contracts.md) and public headers. Older M0–M7 checkpoint notes remain local historical material; they do not override current source or this bounded evidence.
