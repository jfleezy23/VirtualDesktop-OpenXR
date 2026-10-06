# NR GPU regressions

These tests require Windows, the MSVC x64 tools, and a D3D12-capable GPU.

`build-runtime-redirect-regression.cmd` builds a CPU-only actual-DLL negotiation test and isolated success/failure targets. Run `bin/tests/redirect/runtime_redirect_regression.exe <bundle-runtime.dll> bin/tests/redirect/vdxr_redirect_failure_test.dll failure`, then the same command with `vdxr_redirect_success_test.dll success`. A read-only registry detour supplies the test redirect: no registry value, VR instance, session, or graphics device is created. Failed negotiation must unload the target and preserve its error; successful negotiation must retain callable target dispatch.

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
- `nonnr_swapchain_regression`: reversed slice order (`alias`), array/mip copies (`mip`), Vulkan `mutable`/`stencil` metadata, image import `retry`/`vk-retry`, and `ovr-length`/`ovr-create`/`ovr-replacement` failures. Alias/mip and OVR modes require OVRNull. Vulkan dispatch is intercepted; these are not native Vulkan driver tests.
- `nonnr_gl_regression`: `storage`, `cleanup`, `wgl`, `double-fault`, `memory-create`, `texture-create`, `semaphore-create`, and `semaphore-import`. WGL/GL calls are intercepted, with small real D3D shared textures and submission fences. Semaphore failures must stop initialization before a nested timer context can discard the error. OOM cases demonstrate runtime ownership under a controlled provider; they do not promise that a native GL context recovers after OOM.
- `nonnr_input_regression`: `controls`, `float`, `action-paths`, `action-pose`, `query-cache`, `events`, `poll-lock`, `velocity`, `velocity-controls`, `velocity-offsets`, `velocity-invalid`, `pinch-velocity`, `sync-validation`, and `action-set-lifetime`. Velocity, sync-validation, and lifetime modes require OVRNull, whose pose/input exports are intercepted. Invalid-velocity tests inject NaN/infinity, preserve finite pose controls, and check independent validity and offset/base dependencies. Lifetime checks quarantine only the watched action-set allocation to make premature retirement deterministic. Failed-sync tests use live handles and require activation/priorities to survive attachment/path validation errors.
- `nonnr_buffer_regression`: `counts`, `strings`, `string-controls`, and `visibility`, or `all` with OVRNull. Tests cover required count outputs, the string terminator boundary, exact/oversized capacity controls, and independently undersized visibility arrays.

Run fixtures in separate bounded child processes. Require successful completion, a `PASS:` marker, and no `FAIL:` marker. These suites do not write registry settings or require a headset. They establish the specified error paths and data contracts, not performance improvements or exhaustive OpenXR conformance.
