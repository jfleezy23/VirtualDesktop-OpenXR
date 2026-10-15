# Optional stereo frame generation

This x64 experimental integration uses the retained OFXR D3D12 synthesis core
as an optional adjacent DLL, vdxr-frame-generation.dll. It runs after final stereo processing, including NR when enabled, sharpening and upscaling. Each real input receives NR once per eye; synthesized
outputs are not fed through NR again. When NR is enabled, coverage must be at least 90%.
The validated local configuration retains 100% coverage.

## Controls and activation

The existing VDXR NR Settings panel offers **Off**, **OFXR 2X** and **OFXR 3X**.
Selection saves automatically to the 64-bit runtime registry view. Restart the
game to apply a different FG mode. The selector shows the saved request, not a
claim that generation is currently active. Changing FG does not reset NR
settings. Installing the package does not enable FG or change the registry.

Turn Virtual Desktop's headset-side SSW off for OFXR tests. Keep any external
OFXR generation layer off for these comparisons. SSW is a separate 2X mechanism;
this package adds PC-side 2X and 3X rather than stacking generators.

The initial integration accepts one complete color-only stereo projection with
matching RGBA eye dimensions, runtime-owned asynchronous submission and valid
prediction. Depth submission, mixed/overlay layers, static/protected inputs,
unsupported formats/layouts, or enabled NR with coverage below 90% preserve the complete ordinary path. NR-off FG supports DirectX 11 and DirectX 12 without initializing NGX or the NR backend. Missing optional DLL or presentation allocation failure
disables FG for the session and preserves NR. Uncertain GPU ownership stops the
operation while retaining resources; it is never permission to unload live work.

Each synthesized role has its own presentation chain. GPU input-consumption,
output-readiness and host-consumer retirement are separate. Application frame IDs
do not become compositor IDs; the worker owns consecutive native Wait/Begin/End
cycles. Missed deadlines use real frames and reset history. They do not replay
synthetic slots at changed fractions.

Real-frame fallback copies retained final color before waiting for synthesis.
The worker closes next-frame preprocessing admission until all selected current
output copies are queued, then reopens it while retaining the context lease
through the first native handoff. Each prepared role has a distinct SDK image
and authoritative copy-completion fence. Host callers must keep captured source
contents unchanged until the real copy is queued; the runtime admission gate
enforces that contract. Long module stalls can still delay later retirement.
Destination completion uses one retained GPU event, with bounded waits and an
authoritative fence recheck after each wake. This avoids timer polling that can
sleep past a synthetic slot's remaining render lead. An unready or missed group
retains preprocessing admission until retirement rather than provoking a BUSY
capture/reset in the next producer.

## Reproducible builds and bounded checks

Use PowerShell on Windows with Git, CMake, Python and the pinned MSVC/Windows SDK
installed. v145 selects MSVC14.51.36231 and SDK10.0.26100.0; v143 selects
14.44.35207. The normal runtime remains C++17 with precise floating point;
the optional synthesis module uses C++20. Use fresh ignored output directories.

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Build-NrFrameGeneration.ps1 -Toolset v145 -OutputDirectory ./bin/fg-ordinary-example
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Build-NrFrameGeneration.ps1 -Toolset v145 -PrivateRuntime -OutputDirectory ./bin/fg-private-example
python tests/build-fg-runtime-fixtures.py bin/fg-private-example/runtime nr_fg_worker_regression nr_fg_runtime_regression nr_fg_delay_regression
python tests/run-fg-regressions.py --suite all --module-directory bin/fg-private-example/module/Release --runtime-cohort bin/fg-private-example/runtime --backend-template C:/path/to/private-backend --output-directory bin/fg-check-example
~~~

If Visual Studio is installed elsewhere, supply `-MsBuildPath` with its `MSBuild/Current/Bin/MSBuild.exe` path.
The linked fixture builder accepts `--installation` with the same installation's root (containing `VC/` and `MSBuild/`).
The default v143 path is the VS2022 BuildTools installation; the default v145 path is `C:/Tools/VDXR-MSVC145`.

The private backend template must contain the matched local test
LibOVRRT64_1.dll and the installed nvngx_dlssnr.dll. The runner copies these
into an isolated output directory; it does not register a runtime or write real
NR settings. Never install a private runtime. Private cohorts are explicitly
compiled with LOAD_LOCAL_LIBOVR; ordinary cohorts use the VD backend.

Build-time source and artifact receipts are required. A current primary test
source alone is insufficient: included helpers, runtime headers, objects and
module inputs must match. Each child has a 60-second limit and must exit
successfully with a nonempty PASS marker and no failure/assertion markers.
GPU checks require NVIDIA hardware; WARP or initialization failure is not a
passing substitute. Run controlled cadence tests without competing GPU suites.

The retained test suite exercises C/C++ ABI rejection, real moving-pixel 2X/3X,
gamma/alpha, cropped-eye and opposite-eye isolation, D3D11/D3D12 transfer,
blocked consumer/stale ticket, initialization and queue Signal faults,
prediction quantization, actual post-NR/upscaler capture, native role
presentation, late output, active/pending capacity, discard recovery,
metadata discontinuities, shutdown, recoverable startup, later native-slot
faults, allocation rollback and unsupported-composition bypass.
GPU-delay checks preserve the original producer dependency while independently
blocking synthesis, force a later NR-style wait between native submissions,
check S2/B destination readiness and advancing SDK indices, and verify that an
unfinished current NR source remains a dependency. They never perform a
blocking output readback before the deadline observation.
Event checks cover repeated timeouts, stale notifications, native event/wait
failures and an injected post-wait device-removal sentinel. They do not remove
the physical graphics device.

Win32 verification reaches an inherited NR SDK link limitation: this branch
only has NVIDIA's x64 NGX static library. The optional generator is x64 only.
No successful 32-bit NR link is claimed; the installed stock 32-bit VD DLL is
not replaced.

## Quality and performance acceptance

GPU/call-order fixtures do not prove delivery to a physical headset, sustained
game throughput or improved motion-to-photon latency. The source core produces
synthetic colors in the current real frame's camera plane; do not interpolate
the submitted pose to a different plane. Depth-unaware translation and
disocclusions need visual acceptance.

Use VR Paradise with identical scene, refresh, resolution and 100% NR settings
in fresh Off/2X/3X sessions. Verify stereo fusion, face/hair detail in motion,
occlusions and controller response. Record real/generated/repeated/dropped
frames, GPU times, queue age and actual native presentation cadence. Compare
SSW separately with OFXR off. A higher FPS counter alone proves no latency gain.
Headset acceptance remains pending until those observations are recorded.

## Source, licenses and rollback

OFXR is pinned to 07449429c139e5d2070b6f0855ed5323de7cee00
(V443 / 0.2.13.2); FidelityFX is pinned to
c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 (1.1.4).
Retained source manifests record upstream files and hashes.
scripts/prepare-fg-vendor.py emits the initialization ownership derivative
and corresponding patch; vendor source is otherwise kept intact.
The retained LGPL3, companion full GPL3, NVIDIA header notices and AMD notice
accompany the optional module. The full GPL3 text is copied from GNU GCC's
COPYING3. Do not redistribute the installed proprietary NR vendor DLL.

A local deployment package includes ordinary runtime/module, the GUI adapted
to VD's bundled registry prefix, source/derivative receipts and license copies.
Replacing the three files is a sequence of individually atomic Windows
replacements while games are stopped. Verified independent originals are
retained before the first replacement, and failures roll back in reverse order.
Unexpected external contents are preserved for manual recovery. Registry,
runtime/layer registration, vendor DLL, stock32 DLL and shortcut are captured
and compared, never reset by installation. Restore those backed-up files with
games closed to revert; remove a newly added optional DLL if absent originally.
Public commits or distribution require separate authorization.
