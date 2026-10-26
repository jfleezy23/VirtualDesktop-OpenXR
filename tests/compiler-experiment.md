# MSVC 14.51 builds and optional PGO experiments

The experimental NR branch now defaults to MSVC 14.51.36231 (v145) in the x64
project, helpers and CI. The stable branch remains separate. Keep legacy tools
alongside the new ones for explicit comparison builds;
do not reuse objects or PCH files across compiler versions. This is a CPU runtime
compiler experiment: FXC helper shaders and the separate NR vendor module are not
recompiled by changing the C++ toolchain.

The pinned Windows SDK C++/WinRT header includes experimental coroutines under
C++17. `scripts/Experimental-MSVC145.props` provides that compatibility for v145.
It does not change the language standard, floating-point model, instruction
baseline or NR coverage.

After restoring the repository's pinned dependencies, run from its root in
PowerShell, without `CL` or `_CL_` environment overrides. Use a fresh output folder
and select the installed compiler explicitly:

The recipe deliberately pins compiler package **14.51.36231** and Windows SDK
**10.0.26100.0**. Install those components for this comparison.

```powershell
$ErrorActionPreference = 'Stop'
$source = (Get-Location).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$found = @(& $vswhere -latest -version '[18.0,19.0)' -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe')
if ($LASTEXITCODE -ne 0 -or !$found) { throw 'MSVC 14.51 Build Tools were not found' }
$msbuild = $found[0]
$output = Join-Path $source 'bin/compiler-145-releasebundle'
if (Test-Path -LiteralPath $output) { throw 'Use a fresh output folder' }
$objects = Join-Path $output 'obj'
New-Item -ItemType Directory -Force -Path $objects | Out-Null
$commit = & git rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[a-f0-9]{40}$') { throw 'Cannot determine source revision' }
[IO.File]::WriteAllText((Join-Path $objects 'commit.h'), "const char* RuntimeCommitHash = `"$commit`";`n", [Text.Encoding]::ASCII)
$arguments = @(
    'virtualdesktop-openxr/virtualdesktop-openxr.vcxproj', '/t:Rebuild', '/m:2', '/nologo', '/v:normal',
    '/p:Configuration=ReleaseBundle', '/p:Platform=x64', '/p:PlatformToolset=v145',
    '/p:VCToolsVersion=14.51.36231', '/p:WindowsTargetPlatformVersion=10.0.26100.0',
    ("/p:SolutionDir=" + $source.Replace('\', '/') + '/'),
    ("/p:OutDir=" + $output.Replace('\', '/') + '/'),
    ("/p:IntDir=" + $objects.Replace('\', '/') + '/'),
    ("/p:ForceImportBeforeCppTargets=" + (Join-Path $source 'scripts/Experimental-MSVC145.props')),
    '/p:PreferredToolArchitecture=x64', '/p:PreBuildEventUseInBuild=false', '/p:PostBuildEventUseInBuild=false'
)
& $msbuild @arguments
if ($LASTEXITCODE) { throw 'Experimental runtime build failed' }
```

This builds an ordinary optimized release with precise floating point and LTCG.
It does **not** enable AddressSanitizer or the private `LOAD_LOCAL_LIBOVR` backend.
Do not install an instrumented or private-backend test DLL into VD Streamer.

For compiler or optimization comparisons, use identical
source, SDK, helper shader bytes and vendor module. Run the native regressions
described in [nr-hardening.md](nr-hardening.md), keep coverage at 90% or higher,
and perform headset stereo/quality and latency checks separately. Native NR
alpha/depth and parameter checks do not establish RGB equivalence or a speedup.
PGO and other optimization settings remain separate experiments and are off by
default.

## Sample-guided PGO

MSVC 14.51 supports [sample-guided PGO](https://learn.microsoft.com/en-us/cpp/build/sample-profile-guided-optimization?view=msvc-180).
It can optimize the runtime's host C++ code, not the separately supplied NR GPU
module or FXC shader code. Preserve `/fp:precise`, the instruction baseline,
100% NR coverage, model/preset and appearance settings during comparisons.

Use an isolated release cohort with `/GL`, `/O2`, `/Zi` and linker
`/DEBUG /LTCG /SPGO /SPD:<output.spd>`. Retain the exact DLL, PDB and empty SPD,
their SHA-256 hashes, compiler/SDK, source inputs and build defines. Profile the
actual runtime DLL through the OpenXR loader: a fixture linked to runtime objects
in its own EXE cannot train the deployed DLL. Private `LOAD_LOCAL_LIBOVR` profiles
belong only to their private build cohort.

Run the trace collector elevated while leaving the application at normal
permissions; the OpenXR loader ignores `XR_RUNTIME_JSON` in an elevated process.
Keep trace ownership bounded and avoid interfering with another active collector.
[SPTAggregate](https://learn.microsoft.com/en-us/cpp/build/sptaggregate?view=msvc-180)
uses a binary-name filter such as `/binary:virtualdesktop-openxr.dll`, rather
than the DLL's full path. Require nonzero usable samples, the matching symbols,
and appropriate LBR/IP conversion before accepting the resulting profile.
Relink with `/SPGO /SPDIN:<trained.spd>` into another isolated cohort. Never mix
private and ordinary profiles or silently reuse a changed source/tool cohort.

Training should include representative gameplay. Hold out other scenes
or resolutions, keep timing runs free of competing builds/GPU tests, and compare
multiple processes in counterbalanced order after warmup. Separate caller-thread
CPU cost from frame-wall time, GPU inference, presentation and headset latency.
Synthetic private-backend checks establish workflow/correctness only. Sparse
profiles or overlapping/mixed timing results do not justify enabling PGO in the
installed runtime. Record those negative results as well as improvements.
