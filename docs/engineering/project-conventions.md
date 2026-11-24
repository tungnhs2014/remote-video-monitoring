# RVM engineering conventions

These conventions describe the current source organization; they do not create a second ABI or claim compliance with an internal Samsung standard. Current owner instructions, public interfaces and the [system contracts](../architecture/system-contracts.md) control behavior. MISRA C, CERT C, Barr-C and Clean Code inform relevant safety and readability decisions, without a certification claim.

| Layer | Language | Boundary |
|---|---|---|
| HAL facade, Core, runtime, V4L2 and simulated backends | C11 | Public HAL C ABI and backend SPI v2 |
| `camstreamsrc` | C/GObject | Public HAL calls only; no direct V4L2 I/O |
| Controller, Service and gRPC | C++17 | RAII, pipeline/session state and application serialization |
| Linux kernel | Kernel style | Not formatted with RVM userspace rules |

## Structure and names

Public headers declare consumer-facing API and ownership contracts; private state stays in module `src/` headers. The HAL facade forwards calls, Core owns camera state/tokens/rollback, runtime owns module binding/references, and the V4L2 backend divides device, configuration and streaming work by resource. Use `camstream_camera_*` for public HAL operations, `camera_*` for backend SPI callbacks, `v4l2_*` for backend helpers, and the shared `CAM_LOGE/W/I/D` logger. Do not rename ABI or protobuf fields for style.

Keep functions focused on a coherent transition or resource, with blank lines separating useful sections. Split a helper when its responsibility is clear, not at an arbitrary line count. Explain cleanup order, ownership and nonobvious constraints in comments; document selective public API lifetime/error rules with Doxygen.

## Formatting and safety

The repository `.clang-format` targets Allman braces, four-space indentation, eight-space continuation, grouped long signatures and a 140-column userspace limit with clang-format 14. Conditional `extern "C"` header blocks have a documented scoped exception requiring visual review. Keep CMake targets/dependencies/install groups readable; retain Buildroot Makefile and SysV shell conventions. A formatting pass must not alter logic or cleanup order.

One resource has one owner and a defined release path. Check size overflow before arithmetic, capture `errno` immediately after failing syscalls, and roll back in reverse acquisition order. A failed HAL release retains outstanding token ownership. The C++ service must not free HAL state while source callbacks still borrow it; C callbacks must not leak a C++ exception. Logs supplement, rather than replace, HAL statuses, GStreamer flow/bus messages and RPC errors.

For a newcomer, start with [component design](../component-design.md) and [runtime lifecycle](../runtime-lifecycle.md). The [current system contracts](../architecture/system-contracts.md) own ABI and lifecycle rules; historical engineering notes do not describe another production layer.
