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
