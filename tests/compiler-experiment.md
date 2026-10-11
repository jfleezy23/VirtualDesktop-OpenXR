# Opt-in MSVC 14.51 build

The experimental NR branch can be built with MSVC 14.51 while the normal project
and CI continue to use v143. Install the new Build Tools alongside the old ones;
do not reuse objects or PCH files across compiler versions. This is a CPU runtime
compiler experiment: FXC helper shaders and the separate NR vendor module are not
recompiled by changing the C++ toolchain.

The pinned Windows SDK C++/WinRT header includes experimental coroutines under
C++17. `scripts/Experimental-MSVC145.props` opts into that compatibility only when
explicitly imported for v145. It does not change the language standard, floating
point model, instruction baseline, NR coverage, or ordinary build defaults.

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

Before changing the deployed compiler, compare old/new builds with identical
source, SDK, helper shader bytes and vendor module. Run the native regressions
described in [nr-hardening.md](nr-hardening.md), keep coverage at 90% or higher,
and perform headset stereo/quality and latency checks separately. Native NR
alpha/depth and parameter checks do not establish RGB equivalence or a speedup.
PGO and other optimization settings are separate experiments.
