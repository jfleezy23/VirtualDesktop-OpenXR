# Accessibility controller regressions

`accessibility_pose_regression.cpp` exercises the real helper factory and its input
worker through process-local LibOVR provider hooks. No headset, registry changes,
or accessibility configuration installation is needed. The supplied output folder
must exist and be writable; each process removes its own temporary configuration.

Use x64 ReleaseBundle runtime objects from the same source revision and headers
as the fixture. **Disable whole-program optimization for this probe build.** LTCG
can inline the shim entry points past Detours; a timeout in a positive control is
a broken probe, not evidence about the runtime. Keep normal production builds
and these test objects in separate output folders.

From a v143 x64 developer shell, build a fresh probe cohort:

```powershell
$previousCl = $env:CL
try {
    $env:CL = "/GL- $previousCl"
    & scripts/Build-CommunityRuntime.ps1 -Analyze -Configuration ReleaseBundle `
        -Platform x64 -OutputDirectory bin/accessibility-probe `
        -MsBuildPath 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe'
    if ($LASTEXITCODE) { throw 'Probe runtime build failed' }
} finally {
    $env:CL = $previousCl
}
& tests/vr_input_build.cmd -RuntimeObjectDirectory bin/accessibility-probe/obj `
    -TestSource accessibility_pose_regression.cpp
if ($LASTEXITCODE) { throw 'Accessibility fixture build failed' }
foreach ($mode in @('left','right','large','negative','minimum','lost','unavailable')) {
    & bin/vr-input-tests/accessibility_pose_regression.exe $mode bin/accessibility-probe
    if ($LASTEXITCODE) { throw "Accessibility case failed: $mode" }
}
```

`left`, `right`, and `large` preserve valid and high-clamped dominant-hand input
mapping. `negative` and `minimum` reproduce invalid signed indices. `lost` and
`unavailable` exercise an error and qualified success respectively, including
untouched and identity output, last-good stationary pose retention, and fresh
head-tracking recovery. The fixed runtime must pass all seven cases. A baseline
without the fixes fails the missing-head assertion and crashes for negative hands;
the first failed assertion ends its process, so later assertions are only covered
by the passing candidate.

AddressSanitizer runs require compiling the matched runtime objects and fixture
with `/fsanitize=address`, without `/GL`, and linking with the same toolchain's
ASan support. Ordinary native cases above do not require ASan. Neither probe
establishes physical-controller tracking behavior, headset image quality, NR
throughput, or an end-to-end latency improvement.
