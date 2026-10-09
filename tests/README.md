# Stable runtime regressions

These tests exercise the runtime's general input, session, compositor, interop,
and loader contracts. They use no neural rendering SDK or module. Build the
`ReleaseBundle|x64` core runtime first, then link every fixture against that
build's complete object directory. Private headers and runtime objects must
match; objects from another branch or an earlier class layout are incompatible.

```powershell
.\tests\vr_input_build.cmd -RuntimeObjectDirectory <fresh-object-directory> -TestSource nonnr_input_regression.cpp
.\bin\vr-input-tests\nonnr_input_regression.exe cpu
```

The builder accepts the same `-TestSource` option for each suite below. Tests that
use OVRNull take an explicit directory containing `LibOVRRT64_1.dll`; they do not
change the selected system runtime or write registry settings. Run GPU and
backend tests sequentially, in individual bounded processes.

| Suite | Modes | Explicit OVRNull directory required |
|---|---|---|
| `nonnr_async_regression` | `startup`, `error-wait`, `error-begin`, `error-end`, `shutdown-not-initialized`, `control-during-wait`, `wait-retry`, `layers-sync`, `layers-async` | All modes |
| `nonnr_buffer_regression` | `cpu`, `counts`, `strings`, `string-controls`, `visibility`, `all` | `visibility`, `all` |
| `nonnr_compositor_regression` | `lifetime`, `bounds`, `gamma`, `array-alpha`, `missing-backend`, `nt-retry`, `fence-cleanup` | `lifetime` |
| `nonnr_gl_regression` | `storage`, `cleanup`, `wgl`, `double-fault`, `memory-create`, `texture-create`, `semaphore-create`, `semaphore-import` | None |
| `nonnr_depth_alignment_regression` | `d16`, `d24s8`, `d32`, `d32s8`, `all` | All modes; production shader pixels on WARP with intercepted external OVR allocation |
| `nonnr_input_regression` | `cpu`, `controls`, `float`, `action-paths`, `action-pose`, `query-cache`, `action-set-lifetime`, `events`, `poll-lock`, `sync-validation`, `pinch-velocity`, `velocity`, `velocity-controls`, `velocity-offsets`, `velocity-invalid` | Action-set lifetime, sync validation, pinch and all velocity modes |
| `nonnr_preprocess_cache_regression` | `reuse`, `shader-retry`, `buffer-retry`, `cleanup`, `partial-cleanup`, `no-work`, `all` | None; requires native D3D11 Device5/Context4 fence support |
| `nonnr_resolution_regression` | `resize`, `reuse`, `texture`, `srv`, `uav`, `texture-right`, `srv-right`, `uav-right` | All modes; uses D3D11 WARP |
| `nonnr_swapchain_regression` | `alias`, `mip`, `ovr-length`, `ovr-create`, `ovr-replacement`, `mutable`, `stencil`, `retry`, `vk-retry`, `unused-rtvs` | First five modes and `unused-rtvs` |

The five `vr_input_*` suites cover action activation/session reset, callback
draining/constructor unwind, settings publication, visibility event lifetime,
and partial Vulkan cleanup. See [vr_input_README.md](vr_input_README.md) for
their invocation and scope. The watcher late-callback sentinel is a stable
controller setting. Tests replace external dispatch or seed private state where
needed while calling the real production methods.

Build the OVRNull `Release|x64` project before running
`ovr_null_build_regression.cmd current` and
`bin/tests/ovrnull-current/ovr_null_index_regression.exe`. Its default object path
is `obj/x64/Release/OVRNull`; the fixture calls the actual backend producer and
consumer image-index methods.

`build-runtime-redirect-regression.cmd` builds the standalone redirect test
modules, redirect fixture, and oversized-path fixture. Run redirect failure and
success cases with the core runtime DLL and the matching test target DLL. Run
`runtime_path_regression.exe` with an isolated copy of the runtime at a path
longer than `MAX_PATH`. The fixture expects negotiation to reject that path.

FSR/CAS and depth alignment shaders must be rebuilt with their production C++
constant layouts. The generic resolution suite verifies upscaler pixels and
resource recovery. The depth suite verifies all four depth formats, offset and
scaled unequal eye viewports, untouched borders, per-layer cache isolation and
real swapchain cleanup. These fixtures do not establish every crop/gamma
combination or headset latency or appearance.

## Upstream contract regressions

`build-runtime-redirect-regression.cmd` also builds `runtime_negotiation_regression.exe`. Pass the absolute candidate DLL
path to run its 23-case interface/API range and malformed-structure matrix. It suppresses redirect reads inside the test
process, verifies returned dispatch code belongs to that DLL, and never creates a session or writes the registry.
Run the existing redirect success/failure controls separately to validate forwarding and dispatch lifetime.
Append `wide` to either redirect command to test forwarding with the loader interface range `[1,2]`.

Build `nonnr_input_regression.cpp` with `vr_input_build.cmd` and a fresh matching x64 ReleaseBundle object directory.
`sync-set-paths` is CPU-only and checks that one action set cannot inherit another set's declared paths. `sync-scopes` and
`sync-validation` require the explicit OVRNull backend directory as the second argument. They call the real runtime sync,
state-query and space methods with an isolated input provider. Scope coverage includes all input types, aggregate values,
excluded poses/velocities, per-hand priorities, rebinding, wildcard/union entries, unique generations and failed validation.

Build `nonnr_d3d12_flush_regression.cpp` through the same helper on a D3D12-capable GPU. Run separate bounded processes for
`success`, `blocked`, `signal`, `event`, `wait`, `noqueue` and `nofence`. It uses real queue/fence resources with injected API
failures and validates retained resources, retryable destruction, and completion before retirement. It does not require a
headset or exercise NR; a failed drain does not imply that gameplay can resume after partially started session teardown.

See [khronos_README.md](khronos_README.md) for official stereo, pacing, swapchain and input contract checks. Keep native
headset evidence separate from private-backend GPU tests and auto-skipped visual prompts.

## Allocation and GPU timer checks

`nonnr_input_regression` adds `sync-allocations`, `sync-benchmark`, and `sync-many-benchmark`, each taking an explicit
OVRNull directory. The allocation test intercepts the calling thread's real heap allocations, checks a positive control,
and exercises warmed duplicate/wildcard/scope changes and long nonhand path boundaries. Its allocation bound applies to
this fixed provider/profile fixture; tracing, controller rebinding, and backend work can still allocate. Benchmark modes
print CPU distributions and allocation counts without imposing a machine-dependent speed threshold. The many-set fixture
checks 32 unique sets with duplicate requests to expose the retained vectors' linear lookup tradeoff.

Build `nonnr_gpu_timer_regression.cpp` against the same fresh runtime object cohort. `contract` uses a real WARP device and
intercepts GetData to check all three DONOTFLUSH flags, unavailable/error/disjoint handling and reset/non-reset behavior;
it runs in CPU CI. `native` uses a hardware device and a three-slot timer ring with rendered GPU work, reporting available
samples and query cost. Its per-frame submission flush occurs after recording work, independently of timer reads.
`benchmark` retains the native measurements while permitting old flags for baseline comparison. Enforce a child-process
timeout on all modes. Native sample progress is not proof of Virtual Desktop scheduling or headset latency improvement.

## Frame storage and copied-image checks

- `nonnr_async_regression`: readiness and two successive frame IDs, wait/begin/end errors, retry shutdown, event/exit progress during a blocked backend wait, and first-wait retry ownership. `gated-end` holds a successful backend End while another producer waits and checks intact consecutive payloads. `storage-sync` and `storage-async` warm both retained buffers, check zero payload-sized allocations for alternating layer counts, and reject a partial payload before a valid retry. The heap observer has a positive control and excludes backend-spy allocations. All modes require an explicit OVRNull directory.
- `nonnr_copy_preprocess_regression`: real hardware D3D11 pixels through production copy, preprocessing and frame submission, with intercepted backend indexing/commit/End. Modes: `clear`, `premultiply`, `gamma`, `direct`, `blend`, `duplicate`, `disjoint`, `overlap`, `primary-noop`, `viewport-changing`, `flags-changing`, `rotating`, `fresh-release`, `pending`, `regions-stress`, `gamma-stress`, `correction-retry`, `mixed-disjoint`, and `mixed-overlap-coverage`. It checks every pixel, unchanged source data, copy counts, packed regions, equivalent correction coverage, preservation of unaffected coverage after a different transform, and retry after a partially corrected frame. Requires an explicit OVRNull directory. Simultaneous contradictory alpha interpretations of overlapping pixels remain an output-aliasing limitation; this suite does not establish support for them or native compositor latency.
