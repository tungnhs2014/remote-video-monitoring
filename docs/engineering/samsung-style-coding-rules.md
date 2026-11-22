# RVM Engineering Rules — Samsung-Inspired Clean Code (V3 / R1-B)

> **R2-Fixup owner style:** Standalone API declarations are grouped by lifecycle/role with one blank line where it
> improves scanning; operation-table members stay compact. Keep readable prototypes and function-pointer declarations
> on one physical line up to an approximately 140-column practical target. Use native clang-format-14 grouped wrapping
> only for genuinely long declarations. `extern "C"` adds no declaration indentation. The repository `.clang-format`
> defines this layout; C11/C++17 retain Allman braces and four-space indentation. Clang-format 14 indents conditional
> `extern "C"` blocks despite `IndentExternBlock: NoIndent`, so use a scoped formatter exclusion for the declarations
> inside those blocks and review their layout visually.
> These R2-Fixup choices apply to subsequent userspace milestones.

> **R2 update:** The R1-B source split and ownership rules below remain in force. The later owner-approved R2
> decision in `docs/architecture/samsung-style-architecture-decision.md` replaces only the backend registration SPI
> with HAL-owned v2 constructor injection. Public HAL ABI and Core token/state ownership remain unchanged. R2 uses
> explicit `camera_*` operation names, constructor table assignment followed by metadata/version, full loader-side
> validation, the existing `CAM_LOG*` logger, and uppercase CMake command names. This is RVM engineering policy,
> not a claim of Samsung-wide or formal coding-standard compliance.

> Applies to: R2-Fixup Camera HAL C11, V4L2 backend C11, plugin C/GObject and C++17 service/controller
> Status: **R2-FIXUP LOCAL VERIFICATION; OWNER REVIEW PENDING**
> Architectural companion: [`../architecture/samsung-style-architecture-decision.md`](../architecture/samsung-style-architecture-decision.md)

## 1. Intent, authority and review priorities

Use the enterprise engineering characteristics the owner wants from the provided Samsung HDMI/Broadcast examples: **clear external API → private Core → device implementation**, concise calls, one responsibility per component, consistent module-level errors/logs, visible resource ownership, structured cleanup and low cognitive overhead. Do not imitate unfamiliar private macros/names or claim a universal Samsung standard. Do not copy company source.

Priority: **preserved behavior and ABI → lifecycle/concurrency correctness → clear ownership/dependencies → readability → formatting**. R1-B *requires the source split defined in the architecture decision*; it must not be interpreted as merely adding Doxygen or adjusting headers, as happened in R1. The protected system contracts and real HAL/SPI declarations override cosmetic preferences. MISRA C/CERT C/Barr principles are review guidance, **not** a claim of formal certification.

## 2. Formatting, naming and readable flow

| Area | R1-B rule |
|---|---|
| Language | C11 HAL/runtime/V4L2/simulated; C/GObject GStreamer plugin; future C++17 service/controller |
| Formatting | Repository `.clang-format`, **clang-format-14**; 4-space indentation, Allman braces, ~140 columns, readable grouped wrapping and no extern-block declaration indentation |
| Public symbols | Keep **exact** `camstream_camera_*` names, public enum/status numeric values, HAL/SPI signature and exported ABI; no style-driven renames |
| Private symbols | Brief but unambiguous `snake_case`; example `camera_core_start`, `v4l2_prepare_buffers`, `v4l2_read_frame`; local helpers `static` |
| Functions | One cohesive task; consistent early validation and explicit error exit; avoid deep nesting and long parameter lists when a genuine private context already exists |
| Files | Defined by resource owner/change reason, not a numeric line quota; the R1-B §3 file split is mandatory, but no additional speculative sublayers |
| Data | Prefer a single private context per HAL instance/backend instance; avoid global mutable state except the protected process-wide token allocator/runtime registry |
| Comments | Explain contracts, invariants, non-obvious `why`, cleanup order and concurrency; do not narrate a self-explanatory C statement |
| Build | No new framework, third-party dependency, public header installation, library target or PPI `.so` |

### Preferred façade and private Core shape

The following **illustrates structure, not a new ABI**. In the actual code, preserve the exact signatures, statuses and null-argument behavior of the existing functions:

```c
/* libs/camera-hal/src/camera_hal.c — exported entry point */
camstream_camera_status_t camstream_camera_start(camstream_camera* camera)
{
    return camera_core_start(camera);
}

/* libs/camera-hal/src/camera_core.c — the only owner of the state transition */
camstream_camera_status_t camera_core_start(camstream_camera* camera)
{
    /* Existing validation, state transition and backend-call sequence live here. */
    /* Actual implementation must preserve the pre-R1-B behavior verbatim. */
}
```

Use meaningful **private calls that forward to real owner logic**. Do not reproduce the state machine in both façade and Core. Do not add a generic PPI dispatcher or a second function-pointer layer: the versioned backend SPI already provides implementation polymorphism. Private API declarations belong in `src/camera_core.h`; it is not installed.

## 3. Module boundaries and internal structure

### Public HAL / Camera Core / runtime

- `camera_hal.c`: stable public entry points, minimal forwarding, no `struct camstream_camera` definition, no public token array/counter, no backend-specific I/O.
- `camera_core.c`: private camera structure, states, validation, error memory, public/underlying token mapping, atomic lifetime token generator, rollback, pending failed-release cleanup, backend-op ordering and instance cleanup. Do not duplicate validation at multiple levels without a distinct contract reason.
- `camera_core.h`: only cross-translation-unit private declarations and types needed by façade/Core; no installed header and no ABI exposure. Prefer incomplete types where possible.
- `camera_backend_runtime.c/.h`: existing `dlopen`/`dlclose`, registration, version/size checks, client reference and create-reservation state; retain runtime mutex boundaries. Do **not** move this logic into the camera Core.

### V4L2 backend split

- `camera_backend_v4l2.c`: existing backend `ops` table and registration constructor, with callbacks bound to private owner functions; don't reimplement the same operation in both places.
- `v4l2_device.c`: backend instance create/destroy, low-level syscall wrappers including fake/test hook, device open/close, backend-local error logging and device lifetime helpers. Centralize **all** OS calls in the existing testable shim where applicable; do not change operation-specific syscall retry semantics.
- `v4l2_config.c`: enumerate formats/sizes/intervals, validate requested configuration, apply/confirm active format and rational FPS, return existing HAL status and active configuration.
- `v4l2_stream.c`: REQBUFS, MMAP/unmap, QBUF/DQBUF, poll/wait, STREAMON/OFF, acquire/release and buffer state/sequence/token management, rollback in correct order.
- `v4l2_internal.h`: cross-file private `camstream_camera_instance` context and declarations for the three implementation units; no public HAL token mapping, no `Gst*` types, no cyclic public includes. Keep device/config/stream state exactly single-owned.

**Consolidate duplicated mechanisms, not contracts.** A helper belongs where the resource is acquired/used/released. After format/FPS confirmation, `configure()` calls the private stream allocation helper; `v4l2_stream.c` owns `VIDIOC_REQBUFS`, mapping, initial queueing and failure cleanup. This does not justify a new shared library. Preserve current test preprocessor hooks and link each new `.c` into both real and fake V4L2 module targets.

### Source/Plugin and later C++

The current `camstreamsrc` already has a distinct C/GObject contract. Preserve its public binding API and `GstBaseSrc` behavior; clean it only to correct a demonstrable R1-B regression. It borrows a **session-owned** HAL camera, copies padded rows into a separate YUY2 `GstBuffer` and releases the HAL token **before** publishing.

At M5, CameraService alone owns and serializes session HAL calls and module lifetime (C++17 RAII), and GStreamerController owns pipeline/bus/teardown. **No M5 production implementation belongs in R1-B.** Later code follows the same clarity rule without moving HAL ownership into the plugin.

## 4. Ownership, error handling and cleanup

**One resource → one owner → one teardown rule.** On every function, reviewers must identify the owner of allocated memory, FD, mapped region, queue ownership, module reference and frame token on both success and failure.

- Validate null/state/ranges/buffer bounds where the owning layer can do so once. Check integer overflow before multiplication/addition when deriving image sizes, allocation lengths or offsets. Respect ABI version/minimum size checks.
- Capture `errno` **immediately** after a failing syscall, before other calls/logging. Return an existing, meaningful `camstream_camera_status_t`; do not invent duplicate error enums for each file.
- In C, `goto cleanup` is acceptable for a single well-structured reverse-order unwind. Track successful steps explicitly; cleanup only resources actually acquired. Preserve primary failures and independently diagnose cleanup failures.
- On partial V4L2 MMAP or STREAMON failure: no accidental unmap/QBUF of uninitialized memory, no silent claim that a stream started. Respect the existing `streamoff_required`, `buffer_release_required`, mapped/outstanding and reopen flags and their real semantics.
- On an acquired HAL frame, release succeeds **before** an output GstBuffer is published. Failed release retains ownership and blocks normal publication; do not erase/overwrite the pending token or silently convert failure to success.
- The HAL public token allocator is atomic, nonzero, process-lifetime and never reset/reused. Internal V4L2 frame tokens are a different identity domain.
- Module reference and create-reservation accounting remain transactional. Final module unload happens only after backend destruction. Runtime mutex is **never held across** backend callbacks or loader operations.
- All HAL calls on a single instance (including cleanup) require **caller serialization**, even across GStreamer callbacks and service Stop. Do not falsely claim a short state lock solves an in-flight acquire/release race.
- Preserve the current backend `ioctl_device` test behavior. No blanket `EINTR`/`EAGAIN` retry helper, arbitrary timeout extension or automatic USB recovery in R1-B.

| Failure point | Expected behavior preserved |
|---|---|
| Backend partial create | Destroy partially created instance before canceling create reservation; no final unload while callback is live |
| HAL frame validation fails | Roll back backend buffer; if rollback release fails, retain pending cleanup token |
| HAL live release fails | Keep public-token mapping and return error; allow existing safe retry/cleanup path |
| Device errors / recoverable DQBUF flags | Preserve bounded skip/error semantics, including DISCONT after sequence gaps |
| Stream stop/close fails | Preserve existing state, meaningful error, and best-effort final destroy; do not invent success |
| GStreamer flush / stop | No new acquisition after Stop intent; quiesce callbacks, NULL pipeline, resolve tokens, then HAL teardown |

## 5. Logging and documentation

Use the existing `libs/common/include/camstream/common/log.h` macros **as declared** (`CAM_LOGE`, `CAM_LOGW`, `CAM_LOGI`, `CAM_LOGD`) and current module tag convention. Do not introduce `LOG_ERROR`, new logging frameworks, globally shared generic error memory or repeated per-frame `INFO` logging.

- Log at the **owning failure boundary** with operation, saved `errno` where applicable, camera path/buffer index/sequence when useful; do not print full image bytes, credentials or redundant cascades.
- `ERROR`: failed operation / resource state; `WARN`: recoverable driver abnormality; `INFO`: lifecycle summaries; `DEBUG`: detailed trace behind the existing debug option. Avoid spamming on every normal frame.
- GStreamer failures must reach its bus/error handling; a common logger alone does not substitute for `GST_ELEMENT_ERROR`/flow error propagation. Follow actual GLib/GStreamer reference/transfer/lifetime rules.
- Document installed interfaces with selective Doxygen: ownership, state prerequisites, ABI constraints, threading and failure behavior. Private comments focus on **why** ordering/rollback is necessary, not `i++`-style narration.
- Use small consistent blocks in each source: includes → private definitions → focused helpers → operations/API/registration. Avoid arbitrary section banners, extremely long functions and decorative macros.

## 6. Refactor execution, validation and proof

R1-B refactoring is a **single local checkpoint**. Before editing, inspect current branch, staged/unstaged/untracked/ignored docs and complete source/test/CMake paths. Preserve unrelated modifications. Capture a **pre-refactor working-tree baseline**, not merely `HEAD` when R1 changes remain uncommitted.

1. **Map:** record current public entry points, private Core responsibilities, V4L2 call sites, dependency graph and resource-owner/fault-injection seams. Find the minimum complete set of moved production units and all references in tests/CMake.
2. **Refactor:** carry out the mandatory façade/Core and device/config/stream split described in the architecture V3; preserve signatures, ordering, error paths, installed binaries and the working M4 plugin.
3. **Test/build wiring:** update CMake for the HAL Core and **both real and fake V4L2 modules**, using the same production source list if practical; do not accidentally let the fake target call the real device. No new production target, dependency or installation path.
4. **Prove:** host `-Werror` build + full CTest + GStreamer plugin discovery; relevant ASan/UBSan; ARM Buildroot cross-build `-Werror`; pre/post HAL/SPI symbol/layout/API comparison and unchanged `NEEDED`/install paths; `clang-format-14` and `git diff --check`. Report commands and failures, not just a verdict.
5. **Scope:** update only stale architecture/status docs necessary to make the M4 ownership decision and R1-B state coherent. The two V3 Markdown files are authoritative **input** and should not be replaced by a new contradictory design. No BBB connections/deployment; no git commit/push.

**Acceptance is not line-count based.** The façade no longer owns frame-token state; Core is genuinely separated; V4L2 registration, device, configuration and streaming have legible individual owners; deterministic fault injection still works; no ABI drift or behavior change is introduced.

Known limitations must be stated explicitly: historical M4-B BBB PASS precedes R1-B; the owner subsequently reported R1-B BBB functional smoke PASS, while M5 BBB remains **NOT RUN**; `STAGE7-USB-01` USB/MUSB resets are open; ASan/UBSan with `detect_leaks=0` is **not** LeakSanitizer PASS because an existing GLib/GStreamer 16 KiB report remains unresolved.

Report one of **APPROVED / CHANGES REQUIRED / BLOCKED**, a per-file responsibility map, change summary, measured test evidence, ABI parity, remaining risks and exact owner-only BBB reproduction commands. Do not claim "Samsung standard certified", complete system integration or product reliability.
