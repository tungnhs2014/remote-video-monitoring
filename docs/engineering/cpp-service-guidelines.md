# C++17 service and controller guidance

**Scope:** CameraService, GStreamerController, the gRPC adapter/server and C interoperation. The implemented architecture is described in [component design](../component-design.md); the [system contracts](../architecture/system-contracts.md) own ABI and lifetime rules. Historical M0-B proposals are not current application state machines.

## Language and ownership boundary

The HAL, runtime and backends are C11; `camstreamsrc` is C/GObject. Application orchestration and gRPC use C++17. Do not introduce C++20-only library APIs, a second C++ backend abstraction, or a parallel capture owner. CameraService owns the HAL instance and backend module client reference. GStreamerController owns pipeline, bus and teardown. The source borrows HAL; it does not acquire a module reference.

Use RAII for real C++ ownership and a custom deleter matched to each C API. Check GStreamer transfer annotations before moving an element into a bin or releasing a reference. `std::unique_ptr` expresses exclusive ownership; `std::shared_ptr` is justified only by an actual shared lifetime and does not make the pointee thread-safe. A resource-owning class must not accidentally copy a raw FD, `GstObject*` or HAL handle. Delete copy operations or provide correct value semantics; enable move only when a moved-from object remains valid and destructible.

Values, `const T&`, `T&`, pointers and smart pointers have different lifetime meanings. A borrowed reference or `std::string_view` must not outlive its storage. A nullable pointer does not itself prove ownership. `std::move` only permits a move operation; it does not guarantee that one exists.

## Service lifetime and synchronization

The gRPC server and CameraService are process-lived. Configure creates a session; Stop ends it while the server stays available for later RPCs. CameraService holds `operation_mutex_` across each serialized service operation, including its HAL calls; a short state lock or generation comparison alone cannot provide that caller serialization. GStreamer streaming/bus callbacks and gRPC handlers may run on different threads. During Stop, the controller requests source stop, transitions the pipeline to NULL to drive flushing, waits for quiescence, retries any pending frame release and unbinds the source. Only then does CameraService stop/close/destroy HAL and release the final module reference.

If quiescence fails, preserve the HAL context and report Faulted; do not destroy storage still borrowed by a callback. A failed release retains token or pending cleanup ownership. A source `unlock_stop` during teardown must not reopen admission. Backend Runtime releases its registry mutex before loader calls and backend callbacks; loader constructors can re-enter registration. This is distinct from CameraService's operation mutex. Preserve same-instance HAL caller serialization. Document lock order and use condition-variable predicates for waits. Avoid detached threads in resource-owning flows.

Service state includes Idle, Configured, Streaming and Faulted, with requested/active modes and a process-local generation. Configure validates the driver-confirmed active mode with the controller before committing Configured. Start first starts HAL, then the pipeline. Stop from a successful session returns Idle; a new Start needs another Configure. A lost RPC reply or deadline after commit does not undo the operation: clients query GetStatus before retrying. The [gRPC contract](../../apps/grpc-control/README.md) defines duplicate/stale request behavior.

## C/C++ interoperation and errors

Public C declarations use `extern "C"` when included from C++, but no C++ type or exception crosses the C ABI. A C callback preserves its declared return type, including void cleanup. If a C++ callback exists at a C boundary, contain exceptions inside it and map them to the specified status; a pure C11 HAL does not need a bridge for callbacks that violate this rule. Do not return a pointer into a temporary C++ string.

Internal C++ exceptions are permitted where cleanup and status translation are defined. Destructors must not throw. Preserve the primary failure and separately report cleanup failure. Distinguish request validation, state conflict, unavailable device, backend I/O, pipeline/bus error and network/receiver observations; a log message alone is not a result. Stop and shutdown must retain the ownership order even after error.

## Review and verification

- Check C headers compile as C and C++, and compare HAL/SPI ABI where the checkpoint requires it.
- Check every GStreamer full/borrowed reference, bin transfer, bus message, buffer and caps cleanup.
- Exercise Configure/Start/Stop/Status races, duplicate and stale generation, timeout reconciliation, repeated sessions, fault cleanup and server shutdown.
- Verify no C++ exception crosses a C callback and no HAL call overlaps teardown.
- Record host, ARM and BBB evidence separately; CTest or ARM build does not establish remote video playback.

Apply C++ techniques to the actual ownership problem. A custom smart pointer, deep inheritance tree or work queue added only to demonstrate a technique makes the service harder to maintain.
