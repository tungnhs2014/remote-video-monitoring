# M6 gRPC control contract

This is the M6 decision for the single-camera control plane. It supersedes the historical *proposed* RPC wording in
`docs/engineering/grpc-ipc-guidelines.md`; Camera HAL and backend SPI v2 contracts are unchanged. The wire contract is
`proto/camera_control.proto`, package `camstream.control.v1`. Retire fields with `reserved` tags/names; add compatible
fields rather than reusing a tag. The generated C++ is build output, not handwritten application logic.

## Identity, configuration and observations

- The sole logical ID is `camera0`. The server operator supplies its backend module, camstreamsrc plugin and source
  identifier at startup. There is no built-in `/dev/video0` fallback. `ListCameras` reports this configured mapping,
  **not** a hardware enumeration or proof that the device is currently available. `GetCapabilities` opens the source
  through the service-owned HAL operation while Idle, validates the backend's actual configuration list, and returns
  only modes it reports. Unknown IDs return `NOT_FOUND`.
- Pixel format `PIXEL_FORMAT_YUYV` maps to the public HAL's YUYV fourcc; other/unknown enum values are rejected.
  Width, height and rational FPS numerator/denominator must be nonzero and bounded before conversion. `requested`
  is the accepted client request; `active` is the driver-confirmed HAL result after Configure. Neither is delivered
  frame rate. `local_pipeline_frames` counts fakesink handoffs in local mode and JPEG frames observed at the encoder
  output before RTP packetization in M7 network mode. Neither count measures UDP receipt or remote playback.
- `generation` begins at zero for each server process and increments whenever the authoritative service changes
  session state. It does not identify a process across restarts. Every mutating RPC supplies `expected_generation`;
  the service checks it under the same lock as the operation. A stale value returns `FAILED_PRECONDITION` without
  acting. Duplicate Configure with identical mode and current generation returns the stored configuration without
  repeating device I/O. Different Configure requires Stop; duplicate Start fails; Stop from Idle succeeds.
  The field uses a protobuf `oneof`, so an omitted generation is rejected even when the current value is zero.
- A client that loses a Configure or Start reply must call `GetStatus` before retrying. A deadline/cancellation does
  not roll back a committed service operation. `GetStatus` waits for the serialized operation and reports its final
  state and generation. A server restart resets generation; clients reconcile from the new status, not a saved token.

## Limits, lifecycle and exposure

- Server gRPC receive limit: 4 KiB; send limit: 64 KiB. At most 128 HAL modes are serialized. Client strings and
  mode values are validated before service calls. No frame payload or remote media URL appears in this API.
- One synchronous gRPC adapter borrows one long-lived CameraService. Server shutdown first stops accepting RPCs and
  drains handlers, then calls service Stop, then destroys the service. No RPC handler opens HAL or V4L2 directly.
- Default listener is `127.0.0.1:50051` with plaintext transport. Binding a non-loopback address requires an
  explicit `--allow-insecure-lan` option and is intended only for an isolated/trusted lab network. M6 has no
  authentication or TLS policy for untrusted networks; such deployment requires a separate approved design.
- M7 adds an operator-selected server media mode. The default is `local`; `--media-mode rtp-jpeg` requires a numeric
  IPv4 `--media-host` and nonzero `--media-port`. The operator selects a unicast receiver address. The RPC client cannot
  change this destination. `StartStream`
  confirms only the local pipeline state; UDP provides no receiver acknowledgement. The protobuf contract is unchanged.
- Application RPC logs use C++ `steady_clock` milliseconds (`mono_ms`) for approximate correlation with uptime-based
  kernel logs; confirm the target library's clock mapping before exact timestamp comparisons. A successful RPC
  confirms a control-plane operation, not remote video playback.

## Error policy

Malformed modes and unknown enum values return `INVALID_ARGUMENT`; unknown logical ID returns `NOT_FOUND`;
state/generation conflicts return `FAILED_PRECONDITION`; unavailable device/backend returns `UNAVAILABLE`; unexpected
internal errors return `INTERNAL`. A deadline after commit may be reported by gRPC to the client while GetStatus still
shows the committed state. Detailed backend paths stay in local logs; RPC errors use bounded, operation-level text.

Build and test evidence, dependency footprint and manual BBB commands are in
[`docs/development/m6-grpc-control.md`](../../docs/development/m6-grpc-control.md).
