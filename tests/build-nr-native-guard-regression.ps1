param([switch]$Hardware, [string]$OutputDirectory='', [string]$VCToolsDirectory='',
      [ValidateSet('None','NoNativeGuard','EncodedNativeGuard','NoNegativeShared')][string]$ShaderControl='None')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory=Join-Path $repo 'bin/nr-native-standalone' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
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
$source=Join-Path $repo 'virtualdesktop-openxr'
$fxc="$sdk/bin/$sdkVersion/x64/fxc.exe"
$shaderSource=$source
if ($ShaderControl -ne 'None') {
    # Mutate only copied native residual protection; keep exact-black/crop returns.
    $shaderSource=Join-Path $OutputDirectory "control-$ShaderControl"
    New-Item -ItemType Directory -Force -Path $shaderSource | Out-Null
    foreach ($name in @('NrSpatialCommon.hlsli','NrSpatialDownsampleCS.hlsl','NrSpatialResolvePS.hlsl',
                        'NrNativeResolvePS.hlsl','FullScreenQuadVS.hlsl')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination $shaderSource
    }
    $path=Join-Path $shaderSource $(if ($ShaderControl -eq 'NoNegativeShared') { 'NrSpatialCommon.hlsli' } else { 'NrNativeResolvePS.hlsl' })
    $original=[IO.File]::ReadAllText($path)
    if ($ShaderControl -eq 'NoNegativeShared') {
        $changed=$original.Replace('max(min(residual.r, 0), max(min(residual.g, 0), min(residual.b, 0)))', '0.0')
    } elseif ($ShaderControl -eq 'NoNativeGuard') {
        $changed=$original.Replace('ProtectNrResidual(nativeLinear, nativeLinear, residual)', 'residual')
    } else {
        # Wrong confidence units must fail the matched physical-light pair.
        $changed=$original.Replace('ProtectNrResidual(nativeLinear, nativeLinear, residual)',
                                   'ProtectNrResidual(raw.rgb, raw.rgb, residual)')
    }
    if ($changed -eq $original) { throw 'Native guard negative control did not modify its target.' }
    [IO.File]::WriteAllText($path,$changed)
}
foreach($shader in @(@('NrSpatialDownsampleCS','cs_5_0'),@('NrSpatialResolvePS','ps_5_0'),@('NrNativeResolvePS','ps_5_0'),@('FullScreenQuadVS','vs_5_0'))) {
    & $fxc /nologo /Ges /WX /T $shader[1] /E main /Vn "g_$($shader[0])" /Fh "$OutputDirectory\$($shader[0]).h" "$shaderSource\$($shader[0]).hlsl"
    if($LASTEXITCODE) { throw "Shader compilation failed: $($shader[0])" }
}
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /MT /fp:precise /DNOMINMAX /DNR_SPATIAL_TEST_OBSERVER "/I$OutputDirectory" "$PSScriptRoot\nr_native_guard_regression.cpp" "$source\NrSpatialPipeline.cpp" "/Fe:$OutputDirectory\nr_native_guard_regression.exe" "/Fo:$OutputDirectory\\" /link d3d12.lib dxgi.lib d3dcompiler.lib
if($LASTEXITCODE) { throw "GPU fixture compilation failed: $LASTEXITCODE" }
if($Hardware) { & "$OutputDirectory\nr_native_guard_regression.exe" --hardware }
else { & "$OutputDirectory\nr_native_guard_regression.exe" }
if($LASTEXITCODE) { throw "GPU regression failed: $LASTEXITCODE" }
