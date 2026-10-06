# NR GPU regressions

These tests require Windows, the MSVC x64 tools, and a D3D12-capable GPU.

`run-fence-regression.cmd` compiles and runs the production CommandContext against two concurrent GPU queues. Each queue's Flush must wait for its own completion. The original named-event implementation returned prematurely on the first attempt; the unnamed-event implementation passes 32 attempts.

`build-openxr-regression.cmd` builds a black-box OpenXR application. Pass an OpenXR loader DLL and one case: `restart`, `instance-restart`, `transitions`, `pending-resize`, `depth-offset`, `depth-scale`, `unequal`, or `crop-depth-stress`. An optional build-script argument selects an isolated output directory.

`crop-depth-stress` submits exactly 128 frames with fixed LCG seed `0x4e525631` (multiplier 1664525, increment 1013904223, unsigned 32-bit wrap). Color/depth resources remain alive at 768×768 and 384×384. Each eye gets independently generated, in-bounds color extents 384–768 and depth extents 192–384 with independent offsets. Both views omit depth every fourth frame (32 omitted, 96 present). Every frame logs its exact rectangles, exercises acquire/wait/render/release/submit, and checks device removal; teardown must complete. This is a bounded lifecycle stress case with uniform rendered content, not a pixel-quality or latency oracle. It does not vary formats, sample counts, or depth presence between the two eyes. The parent runner sets 90%/100% NR coverage and a process timeout; this harness does not alter settings.

Set `XR_RUNTIME_JSON` to a private runtime manifest, enable NR for the test, and keep coverage at 90% or 100%. Run unelevated: OpenXR ignores environment-selected runtimes for elevated processes. Preserve and restore the prior NR settings.

For tests without a headset, build the runtime with the existing `LOAD_LOCAL_LIBOVR` compiler definition and put the repository's Release x64 OVRNull `LibOVRRT64_1.dll` beside it. Supply the NR DLL separately; it is not part of this repository. This changes backend selection only; the tests still exercise real GPU textures, production NR resource management, and NVIDIA feature evaluation.

The cases cover depth removal, format replacement, resolution and MSAA changes, unequal viewports, independent depth offsets/extents, repeated sessions, DLL unload/reload, and resource replacement while input swapchains remain alive. An external runner must enforce a timeout; the original code crashes or hangs. A completed frame or a zero exit alone is insufficient: require the `PASS: <case>` marker and restored settings.

OVRNull-backed results establish resource/lifecycle behavior. They do not establish headset image quality, streaming latency, or game-specific performance.
