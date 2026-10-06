# NR GPU regressions

These tests require Windows, the MSVC x64 tools, and a D3D12-capable GPU.

`run-fence-regression.cmd` compiles and runs the production CommandContext against two concurrent GPU queues. Each queue's Flush must wait for its own completion. The original named-event implementation returned prematurely on the first attempt; the unnamed-event implementation passes 32 attempts.

`build-openxr-regression.cmd` builds a black-box OpenXR application. Pass an OpenXR loader DLL and one case: `restart`, `instance-restart`, `transitions`, `pending-resize`, `depth-offset`, `depth-scale`, or `unequal`.

Set `XR_RUNTIME_JSON` to a private runtime manifest, enable NR for the test, and keep coverage at 90% or 100%. Run unelevated: OpenXR ignores environment-selected runtimes for elevated processes. Preserve and restore the prior NR settings.

For tests without a headset, build the runtime with the existing `LOAD_LOCAL_LIBOVR` compiler definition and put the repository's Release x64 OVRNull `LibOVRRT64_1.dll` beside it. Supply the NR DLL separately; it is not part of this repository. This changes backend selection only; the tests still exercise real GPU textures, production NR resource management, and NVIDIA feature evaluation.

The cases cover depth removal, format replacement, resolution and MSAA changes, unequal viewports, independent depth offsets/extents, repeated sessions, DLL unload/reload, and resource replacement while input swapchains remain alive. An external runner must enforce a timeout; the original code crashes or hangs. A completed frame or a zero exit alone is insufficient: require the `PASS: <case>` marker and restored settings.

OVRNull-backed results establish resource/lifecycle behavior. They do not establish headset image quality, streaming latency, or game-specific performance.
