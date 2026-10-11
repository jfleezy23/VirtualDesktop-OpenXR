# NR completion and depth regressions

These tests use native Direct3D queues/resources and a process-private OVRNull backend.
They require Windows x64, the repository's pinned dependencies, v143 C++ tools, and
the locally supplied NR vendor module on supported hardware. They do not change
installed runtimes, application settings, headset routing, or NR coverage.

Build the ordinary runtime into a fresh output directory using
`scripts/Build-CommunityRuntime.ps1`. Build a second **test-only** runtime from the
same source/headers with `LOAD_LOCAL_LIBOVR` defined, in another fresh directory.
This definition selects the supplied OVRNull instead of the installed VD backend.
Never install this test runtime.

For the second build, the compiler's `CL` environment variable can supply the test
definition without editing project files. Restore it even if the build fails:

```powershell
$previousCl = $env:CL
try {
    $env:CL = "/DLOAD_LOCAL_LIBOVR $previousCl"
    & scripts/Build-CommunityRuntime.ps1 -Configuration ReleaseBundle -Platform x64 `
        -OutputDirectory bin/nr-test-private
    if ($LASTEXITCODE) { throw 'Private runtime build failed' }
} finally {
    $env:CL = $previousCl
}
```

Place the x64 OVRNull `LibOVRRT64_1.dll`, the local `nvngx_dlssnr.dll`, and that
test-only runtime DLL together. OVRNull comes from this repository's `OVRNull`
project; the NR module is a separate input and must not be committed. Pass the
matching object directories and private backend folder:

```powershell
& tests/run-nr-hardening.ps1 `
    -RuntimeObjectDirectory bin/community/build/x64/ReleaseBundle/obj `
    -PrivateRuntimeObjectDirectory bin/nr-test-private/obj `
    -PrivateBackendDirectory bin/nr-test-private
```

Every runtime-linked executable must use headers and objects from the same source
revision. Freeze baseline headers before editing private structures or signatures;
do not combine baseline objects with candidate headers. Test output stays under
ignored `bin/` directories.

The real-NR fixture dispatches through its linked runtime objects. The companion
runtime DLL is a private-backend build artifact, not its OpenXR dispatch target.
The result manifest records both that companion hash and the exercised fixture
hash, together with the supplied object-cohort paths.

## What is checked

- `nr_command_context_regression`: failed submission/recovery signals, failed
  event registration and wait, device removal before recycling and after a real
  successful wait, retained command-list/allocator ownership, original-list reuse,
  and no redundant event registration after completed work. Pointer-filtered hooks
  inject errors into actual native calls. Lifetime probes do not own GPU objects.
  The GPU gate is released and the worker joined before reporting failure.
- `nr_retirement_regression`: input retirement waits for actual NR-queue work with
  no downstream dependency; failed drains preserve swapchain membership/imports
  for retry. A separate fixture owner keeps GPU resources safe in failing baseline
  controls. This tests the state after a missing output dependency, not the actual
  D3D11 `Wait` failure or final resource destruction timing.
- `nr_input_toggle_regression`: retains the original seven OFF/ON/OFF import/cache
  frames. Metadata/writes/controls modes add five NR frames: both-eye depth with
  opposite conventions, absent depth, right-only and left-only depth, then restored
  depth with the opposite convention. Partial-eye cases are robustness checks;
  conforming OpenXR depth submissions attach metadata to all views.
- Real NR evaluations at 90% and 100% validate per-eye feature/input identity,
  parameter crop, complete optional-depth clearing, and five depth setter calls per
  eye. A forwarding observer is installed after both real features are warm;
  vendor evaluation receives its original allocated parameter object. Observers
  are restored before normal or exception teardown.

Baseline metadata and writes modes should fail their targeted assertions while
controls mode passes. Baseline command signal/removed/postwait/complete and both
retirement cases should fail; event and wait modes are retained passing controls.
All candidate modes should pass. Nonempty `PASS:` output, exit status, assertion
markers, and bounded process completion are checked together.

The original fence fixture additionally tests 32 independent concurrent waits.
NR-on color readback samples alpha, and native depth readback samples known
geometry. These tests do **not** establish NR image quality, RGB equivalence,
headset compatibility, or an end-to-end latency improvement. Visual evaluation
and comparable headset timing remain separate checks.
