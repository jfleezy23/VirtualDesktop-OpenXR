param(
    [string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$RuntimeObjectDirectory,
    [Parameter(Mandatory=$true)][string]$CaseName,
    [string]$OvrNull,
    [string]$VCToolsDirectory,
    [switch]$CandidateSpatial,
    [ValidateSet('None','DropUncertainOwnership','IgnoreWaitFailure')][string]$NegativeControl = 'None',
    [switch]$Run
)
$ErrorActionPreference = 'Stop'
function Assert-RuntimeResult {
    param([string]$Mode, [int]$ExitCode, [string]$Stdout, [string]$Stderr)
    if ($ExitCode -ne 0) { throw "$Mode failed with exit $ExitCode" }
    $lines = @(($Stdout + "`n" + $Stderr) -split '\r?\n')
    $pass = '^PASS: public runtime ' + [regex]::Escape($Mode) + '; native output/identity pixels/depth intact; creates=\d+ releases=\d+$'
    if (@($lines | Where-Object { $_ -match $pass }).Count -ne 1) {
        throw "$Mode did not emit exactly one complete public-runtime PASS marker."
    }
    if (@($lines | Where-Object { $_ -match 'ASSERTION FAILED:' }).Count) {
        throw "$Mode emitted an assertion failure despite its exit status."
    }
    $failures = @($lines | Where-Object { $_.TrimStart().StartsWith('FAIL:') })
    $faultModes = @('evaluate-failure','create-failure','signal-failure','wait-failure')
    if ($faultModes -contains $Mode) {
        if ($failures.Count -ne 1 -or $failures[0] -cne 'FAIL: xrEndFrame(session, &end): -2') {
            throw "$Mode must emit exactly its one expected injected xrEndFrame failure."
        }
    } elseif ($failures.Count) {
        throw "$Mode emitted an unexpected failure."
    }
}
$repo = Split-Path -Parent $PSScriptRoot
if (!$SourceDirectory) { $SourceDirectory = Join-Path $repo 'virtualdesktop-openxr' }
if ($CaseName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Use a simple new case name.' }
$output = Join-Path $repo "bin/nr-spatial-runtime/$CaseName"
if (Test-Path -LiteralPath $output) { throw 'Use a fresh case output; do not mix build cohorts.' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$RuntimeObjectDirectory = (Resolve-Path -LiteralPath $RuntimeObjectDirectory).Path
# Select the pinned tools, allowing an explicitly supplied directory or a Native Tools environment.
$requiredToolset = '14.51.36231'
$toolCandidates = @()
if ($VCToolsDirectory) {
    $toolCandidates = @($VCToolsDirectory)
} else {
    if ($env:VCToolsInstallDir) { $toolCandidates += $env:VCToolsInstallDir }
    $currentCompiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($currentCompiler) {
        $candidate = $currentCompiler.Source
        1..4 | ForEach-Object { $candidate = Split-Path -Parent $candidate }
        $toolCandidates += $candidate
    }
    if ($env:SystemDrive) {
        $toolCandidates += Join-Path $env:SystemDrive "Tools/VDXR-MSVC145/VC/Tools/MSVC/$requiredToolset"
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installations = @(& $vswhere -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        foreach ($installation in $installations) {
            $toolCandidates += Join-Path $installation "VC/Tools/MSVC/$requiredToolset"
        }
    }
}
$vc = $null
foreach ($candidate in $toolCandidates) {
    $candidate = $candidate.TrimEnd('\','/')
    $compiler = Join-Path $candidate 'bin/Hostx64/x64/cl.exe'
    if ((Split-Path -Leaf $candidate) -ne $requiredToolset -or !(Test-Path -LiteralPath $compiler)) { continue }
    # Microsoft services the compiler binary inside the pinned toolset directory.
    $compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.ProductVersion
    if ($compilerVersion -notmatch '^14\.51\.(\d+)' -or [int]$Matches[1] -lt 36231) { continue }
    $vc = (Resolve-Path -LiteralPath $candidate).Path
    break
}
if (!$vc) { throw "MSVC $requiredToolset x64 tools are required; supply -VCToolsDirectory or use their Native Tools environment." }
$sdk = if ($env:WindowsSdkDir) { $env:WindowsSdkDir } else { Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10' }
$sdkVersion = '10.0.26100.0'
if (!(Test-Path -LiteralPath "$sdk/Include/$sdkVersion/um/Windows.h") -or !(Test-Path -LiteralPath "$sdk/Lib/$sdkVersion/um/x64/d3d12.lib")) {
    throw "Windows SDK $sdkVersion headers and x64 libraries are required."
}
$env:PATH = "$vc/bin/Hostx64/x64;$env:PATH"
$env:INCLUDE = "$vc/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/winrt;$sdk/Include/$sdkVersion/cppwinrt"
$env:LIB = "$vc/lib/x64;$sdk/Lib/$sdkVersion/ucrt/x64;$sdk/Lib/$sdkVersion/um/x64"
Write-Output "Toolchain: MSVC $requiredToolset (compiler $compilerVersion); Windows SDK $sdkVersion"
# Reuse all retained runtime include/define/link options verbatim. Only relocate
# its outputs and select this test's source; frozen baseline headers stay first.
$template = [IO.File]::ReadAllText("$repo/tests/vr_input_build.ps1")
$template = $template.Replace('$repo = Split-Path -Parent $PSScriptRoot', ('$repo = ''' + $repo.Replace("'","''") + ''''))
$template = $template.Replace('$output = Join-Path $repo ''bin/vr-input-tests''', ('$output = ''' + $output.Replace('\','/').Replace("'","''") + ''''))
$template = $template.Replace('(Join-Path $PSScriptRoot $TestSource)', ("'" + $repo.Replace("'","''") + "/tests/nr_spatial_runtime_regression.cpp'"))
if ($CandidateSpatial) {
    $template = $template.Replace('if ($BaselineWatcher)', '$compileOptions += ''/DVDXR_NR_SPATIAL_CANDIDATE''; if ($BaselineWatcher)')
}
$privateSystem = @'
$privateSystemObject = Join-Path $output 'private-system.obj'
& cl.exe @compileOptions /DLOAD_LOCAL_LIBOVR (Join-Path $SourceDirectory 'system.cpp') "/Fo$privateSystemObject"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne 'system.obj' })
$objects += $privateSystemObject
if ($RecompileInputSources) {
'@
$template = $template.Replace('if ($RecompileInputSources) {', $privateSystem)
if ($NegativeControl -ne 'None') {
    if (!$CandidateSpatial) { throw 'Spatial lifecycle controls require candidate objects and headers.' }
    # Compile a changed private copy against the same matching host cohort.
    # Production source stays untouched; these runs must fail behavior assertions.
    $controlSource = Join-Path $output 'dlssnr_spatial-control.cpp'
    $sourceText = [IO.File]::ReadAllText("$SourceDirectory/dlssnr_spatial.cpp")
    if ($NegativeControl -eq 'DropUncertainOwnership') {
        $old = 'm_nrSpatialNeedsDrain = true;'
        $new = 'm_nrSpatialNeedsDrain = false; // Negative control: forget uncertain submission ownership.'
    } else {
        $old = 'CHECK_HRCMD(m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fence));'
        $new = 'm_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fence); // Negative control: swallow consumer failure.'
    }
    if (!$sourceText.Contains($old)) { throw 'Negative control target changed; inspect current source before adapting.' }
    [IO.File]::WriteAllText($controlSource, $sourceText.Replace($old,$new))
    $privateSpatial = @'
$privateSpatialObject = Join-Path $output 'private-spatial-control.obj'
& cl.exe @compileOptions (Join-Path $output 'dlssnr_spatial-control.cpp') "/Fo$privateSpatialObject"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne 'dlssnr_spatial.obj' })
$objects += $privateSpatialObject
if ($RecompileInputSources) {
'@
    $template = $template.Replace('if ($RecompileInputSources) {',$privateSpatial)
}
$build = Join-Path $output 'retained-build.ps1'
[IO.File]::WriteAllText($build, $template)
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $build -SourceDirectory $SourceDirectory -RuntimeObjectDirectory $RuntimeObjectDirectory -TestSource nr_spatial_runtime_regression.cpp -Toolset v145 *> "$output/build.log"
if ($LASTEXITCODE) { Get-Content "$output/build.log"; throw "Runtime fixture build failed: $LASTEXITCODE" }
$backend = Join-Path $output 'backend'
New-Item -ItemType Directory -Path $backend | Out-Null
& cl.exe /nologo /std:c++17 /EHsc /W3 /MT /O2 /LD /DNDEBUG /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR "/I$repo/external/DLSS/include" "/I$repo/packages/Detours.4.0.1/lib/native/include" "$repo/tests/nr_test_module.cpp" "/Fo$output/nr_test_module.obj" "/Fe$backend/nvngx_dlssnr.dll" /link "$repo/packages/Detours.4.0.1/lib/native/libs/x64/detours.lib" *> "$output/facade-build.log"
if ($LASTEXITCODE) { Get-Content "$output/facade-build.log"; throw "Private facade build failed: $LASTEXITCODE" }
$inputs = @("$repo/tests/nr_spatial_runtime_regression.cpp", "$repo/tests/nr_input_toggle_regression.cpp", "$SourceDirectory/runtime.h", "$SourceDirectory/dlssnr.cpp")
if (Test-Path -LiteralPath "$SourceDirectory/dlssnr_spatial.cpp") { $inputs += "$SourceDirectory/dlssnr_spatial.cpp" }
if ($NegativeControl -ne 'None') { $inputs += $controlSource }
$inputs += @(Get-ChildItem -LiteralPath $RuntimeObjectDirectory -Filter '*.obj' | ForEach-Object FullName)
$inputs | ForEach-Object { Get-FileHash -Algorithm SHA256 -LiteralPath $_ } | ConvertTo-Json -Depth 3 | Set-Content "$output/inputs-sha256.json"
Write-Output "PASS: runtime fixture/source facade built using MSVC14.51; output=$output"
if (!$Run) { exit 0 }
if (!$OvrNull) { throw 'Run requires the explicitly selected source OVRNull DLL.' }
Copy-Item -LiteralPath $OvrNull -Destination "$backend/LibOVRRT64_1.dll"
$modes = if ($CandidateSpatial) { @('native100', 'sequence', 'evaluate-failure', 'create-failure', 'signal-failure', 'wait-failure') } else { @('native100', 'sequence') }
if ($NegativeControl -eq 'DropUncertainOwnership') { $modes = @('signal-failure') }
if ($NegativeControl -eq 'IgnoreWaitFailure') { $modes = @('wait-failure') }
foreach ($mode in $modes) {
    $state = Join-Path $output "state-$mode"
    New-Item -ItemType Directory -Path $state | Out-Null
    $arguments = @('"' + $backend + '"', '"' + $state + '"', $mode)
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = "$output/nr_spatial_runtime_regression.exe"
    $info.Arguments = $arguments -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $info.RedirectStandardError = $true
    $child = [Diagnostics.Process]::Start($info)
    $stdout = $child.StandardOutput.ReadToEndAsync()
    $stderr = $child.StandardError.ReadToEndAsync()
    if (!$child.WaitForExit(90000)) { $child.Kill(); throw "$mode exceeded the bounded90-second runtime test." }
    $stdoutText = $stdout.GetAwaiter().GetResult()
    $stderrText = $stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText("$output/$mode.stdout.log", $stdoutText)
    [IO.File]::WriteAllText("$output/$mode.stderr.log", $stderrText)
    try {
        Assert-RuntimeResult -Mode $mode -ExitCode $child.ExitCode -Stdout $stdoutText -Stderr $stderrText
    } catch {
        Write-Output $stderrText
        throw
    }
    Write-Output (($stdoutText -split '\r?\n' | Where-Object { $_ -match '^PASS: public runtime ' }) -join "`n")
}
