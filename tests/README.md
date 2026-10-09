# NR GPU regressions

These tests require Windows, the MSVC x64 tools, and a D3D12-capable GPU.

`build-runtime-redirect-regression.cmd` builds a CPU-only actual-DLL negotiation test and isolated success/failure targets. Run `bin/tests/redirect/runtime_redirect_regression.exe <bundle-runtime.dll> bin/tests/redirect/vdxr_redirect_failure_test.dll failure`, then the same command with `vdxr_redirect_success_test.dll success`. A read-only registry detour supplies the test redirect: no registry value, VR instance, session, or graphics device is created. Failed negotiation must unload the target and preserve its error; successful negotiation must retain callable target dispatch.

`build-ovr-bridge-regression.cmd` builds test-only Oculus/Revive/injector markers and a CPU-only actual-DLL instance
fixture. Run `bin/tests/ovr-bridge/runtime_ovr_bridge_regression.exe <bundle-runtime.dll> <absolute-marker-directory> <mode>`
in separate bounded processes for `native`, `native-own`, `bridge`, `bridge-own`, `late-bridge`, `unload-bridge`, and
`bridge-no-plugin`. Require exit success and `PASS:`. Synthetic registry reads disable the fixture's watcher and isolate
its settings without writes. The fixture checks injector loading, fake-event creation, application classification,
unrelated event forwarding, 16 event-hook cycles in bridge cases, and actual runtime unload. Native cases preserve the
existing detection behavior. Preloaded bridge cases must avoid redundant VDXR hooks and injection.

The bridge hook lives in the fixture executable. `late-bridge` and `unload-bridge` change marker presence to check teardown
ownership; they do not reproduce real late Revive hook stacking or vendor injector side effects. The supported guard
covers a bridge loaded before VDXR. These tests do not call `xrGetSystem`, create a backend session, or establish a game
startup fix. Never install the marker DLLs beside a game or runtime. For an additional local CPU integration check,
an isolated fixture directory may contain the separately obtained real ReviveXR DLL and its dependencies in place of
the bridge marker; use the three `bridge`, `bridge-own`, and `bridge-no-plugin` modes. Do not redistribute those binaries
with this fixture.

`build-module-failure-regression.cmd` also builds `runtime_path_regression.exe`. Pass an isolated copied runtime's fully qualified Windows path longer than MAX_PATH, using an extended path when needed. Actual negotiation must return initialization failure with no dispatch pointer, rather than overflowing its filename buffer. This checks safe rejection, not general long-path support.

`run-fence-regression.cmd` compiles and runs the production CommandContext against two concurrent GPU queues. Each queue's Flush must wait for its own completion. The original named-event implementation returned prematurely on the first attempt; the unnamed-event implementation passes 32 attempts.

`build-openxr-regression.cmd` builds a black-box OpenXR application. Pass an OpenXR loader DLL and one case: `restart`, `instance-restart`, `transitions`, `pending-resize`, `depth-offset`, `depth-scale`, `unequal`, or `crop-depth-stress`. An optional build-script argument selects an isolated output directory.

`crop-depth-stress` submits exactly 128 frames with fixed LCG seed `0x4e525631` (multiplier 1664525, increment 1013904223, unsigned 32-bit wrap). Color/depth resources remain alive at 768×768 and 384×384. Each eye gets independently generated, in-bounds color extents 384–768 and depth extents 192–384 with independent offsets. Both views omit depth every fourth frame (32 omitted, 96 present). Every frame logs its exact rectangles, exercises acquire/wait/render/release/submit, and checks device removal; teardown must complete. This is a bounded lifecycle stress case with uniform rendered content, not a pixel-quality or latency oracle. It does not vary formats, sample counts, or depth presence between the two eyes. The parent runner sets 90%/100% NR coverage and a process timeout; this harness does not alter settings.

Set `XR_RUNTIME_JSON` to a private runtime manifest, enable NR for the test, and keep coverage at 90% or 100%. Run unelevated: OpenXR ignores environment-selected runtimes for elevated processes. Preserve and restore the prior NR settings.

For tests without a headset, build the runtime with the existing `LOAD_LOCAL_LIBOVR` compiler definition and put the repository's Release x64 OVRNull `LibOVRRT64_1.dll` beside it. Supply the NR DLL separately; it is not part of this repository. This changes backend selection only; the tests still exercise real GPU textures, production NR resource management, and NVIDIA feature evaluation.

The cases cover depth removal, format replacement, resolution and MSAA changes, unequal viewports, independent depth offsets/extents, repeated sessions, DLL unload/reload, and resource replacement while input swapchains remain alive. An external runner must enforce a timeout; the original code crashes or hangs. A completed frame or a zero exit alone is insufficient: require the `PASS: <case>` marker and restored settings.

OVRNull-backed results establish resource/lifecycle behavior. They do not establish headset image quality, streaming latency, or game-specific performance.

## Non-NR runtime regressions

Build a ReleaseBundle x64 runtime, then link each `nonnr_*_regression.cpp` against its object directory:

```cmd
tests\vr_input_build.cmd -RuntimeObjectDirectory bin\nonnr-reviewed\obj -TestSource nonnr_async_regression.cpp
bin\vr-input-tests\nonnr_async_regression.exe startup C:\path\to\OVRNull-directory
```

The headers and runtime objects must come from the same build. A private class layout change requires rebuilding every linked runtime object and fixture; linking current headers against older objects does not produce valid regression evidence. The build script also accepts `-SourceDirectory` for a matching frozen header set when testing older objects.

The suites exercise real runtime methods, with controlled external providers:

- `nonnr_async_regression`: readiness and two successive frame IDs, wait/begin/end errors, retry shutdown, event/exit progress during a blocked backend wait, and first-wait retry ownership. All modes require an explicit OVRNull directory.
- `nonnr_compositor_regression`: swapchain destruction (`lifetime`, requires OVRNull), alpha `bounds`, mapped `gamma`, untouched second array slice (`array-alpha`), `missing-backend`, retained NT handles (`nt-retry`), and `fence-cleanup`. Pixel/handle cases use a real D3D11 device; Vulkan/GL format metadata is injected.
- `nonnr_preprocess_cache_regression`: `reuse`, `shader-retry`, `buffer-retry`, `cleanup`, `partial-cleanup`, `no-work`, or `all`. Native D3D11 creation counters verify one shader/buffer call across repeated alpha preprocessing and independent retry of a missing resource. Queued pixel checks vary viewport, clear/premultiply and sRGB constants; cleanup checks native device ownership after recreation. Requires hardware D3D11 Device5/Context4 with fence support; no backend directory argument. Call counts establish removed creation work, not CPU time or headset latency.
- `nonnr_swapchain_regression`: reversed slice order (`alias`), array/mip copies (`mip`), Vulkan `mutable`/`stencil` metadata, image import `retry`/`vk-retry`, and `ovr-length`/`ovr-create`/`ovr-replacement` failures. Alias/mip and OVR modes require OVRNull. Vulkan dispatch is intercepted; these are not native Vulkan driver tests.
- `nonnr_gl_regression`: `storage`, `cleanup`, `wgl`, `double-fault`, `memory-create`, `texture-create`, `semaphore-create`, and `semaphore-import`. WGL/GL calls are intercepted, with small real D3D shared textures and submission fences. Semaphore failures must stop initialization before a nested timer context can discard the error. OOM cases demonstrate runtime ownership under a controlled provider; they do not promise that a native GL context recovers after OOM.
- `nonnr_input_regression`: `controls`, `float`, `action-paths`, `action-pose`, `query-cache`, `events`, `poll-lock`, `velocity`, `velocity-controls`, `velocity-offsets`, `velocity-invalid`, `pinch-velocity`, `sync-validation`, and `action-set-lifetime`. Velocity, sync-validation, and lifetime modes require OVRNull, whose pose/input exports are intercepted. Invalid-velocity tests inject NaN/infinity, preserve finite pose controls, and check independent validity and offset/base dependencies. Lifetime checks quarantine only the watched action-set allocation to make premature retirement deterministic. Failed-sync tests use live handles and require activation/priorities to survive attachment/path validation errors.
- `nonnr_buffer_regression`: `counts`, `strings`, `string-controls`, and `visibility`, or `all` with OVRNull. Tests cover required count outputs, the string terminator boundary, exact/oversized capacity controls, and independently undersized visibility arrays.

Run fixtures in separate bounded child processes. Require successful completion, a `PASS:` marker, and no `FAIL:` marker. These suites do not write registry settings or require a headset. They establish the specified error paths and data contracts, not performance improvements or exhaustive OpenXR conformance.

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
