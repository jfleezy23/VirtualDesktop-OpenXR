# Run from an x64 Native Tools prompt with the runtime's MSVC toolset and Windows SDK.
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '../bin/nr-before-sr-tests'))
$ErrorActionPreference = 'Stop'
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Run from an x64 Native Tools prompt.' }
$compiler = (Get-Command cl.exe -CommandType Application).Source
$repo = Split-Path -Parent $PSScriptRoot
$output = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$flags = @('/nologo', '/std:c++17', '/EHsc', '/O2', '/MT', '/W4', '/WX', '/fp:precise', '/DNOMINMAX',
           '/external:W0', "/external:I$repo/external/OpenXR-SDK/include", "/external:I$repo/external/DLSS/include",
           "/external:I$repo/external/LibOVR/Include")
$fixtures = @('nr_before_sr_policy', 'nr_before_sr_ngx', 'nr_before_sr_recording', 'nr_before_sr_pipeline',
              'nr_before_sr_layers', 'nr_before_sr_stereo', 'nr_before_sr_history_bank', 'nr_fg_hud_layout')
foreach ($fixture in $fixtures) {
    $name = "${fixture}_regression"
    $exe = Join-Path $output "$name.exe"
    & $compiler @flags "$PSScriptRoot/$name.cpp" "/Fo$output/$name.obj" "/Fe$exe"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $name (exit $LASTEXITCODE)." }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Regression failed for $name (exit $LASTEXITCODE)." }
}
Write-Output "PASS: $($fixtures.Count) before-SR and HUD CPU regressions."
