param([switch]$PolicyOnly, [switch]$Hardware, [string]$OutputDirectory='', [string]$VCToolsDirectory='',
      [ValidateSet('None','ZeroResidual','EncodedArea','NoTransition','CropDepthAsFullEye','NoShadow','NoChromaToe','SourceOnly','NoNegativeShared')][string]$ShaderControl='None')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory=Join-Path $repo 'bin/nr-spatial-standalone' }
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
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /MT /fp:precise /DNOMINMAX "$PSScriptRoot\nr_spatial_policy_regression.cpp" "/Fe:$OutputDirectory\nr_spatial_policy_regression.exe" "/Fo:$OutputDirectory\nr_spatial_policy_regression.obj"
if($LASTEXITCODE) { throw "Policy compilation failed: $LASTEXITCODE" }
& "$OutputDirectory\nr_spatial_policy_regression.exe"
if($LASTEXITCODE) { throw "Policy regression failed: $LASTEXITCODE" }
if($PolicyOnly) { exit 0 }
$fxc="$sdk\bin\$sdkVersion\x64\fxc.exe"
$shaderSource=$source
if($ShaderControl -ne 'None') {
    # Negative controls compile changed copies, never alter the production files.
    $shaderSource=Join-Path $OutputDirectory "control-$ShaderControl"
    New-Item -ItemType Directory -Force -Path $shaderSource | Out-Null
    foreach($name in @('NrSpatialCommon.hlsli','NrSpatialDownsampleCS.hlsl','NrSpatialResolvePS.hlsl','NrNativeResolvePS.hlsl','FullScreenQuadVS.hlsl')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination $shaderSource
    }
    if($ShaderControl -eq 'ZeroResidual') {
        $path=Join-Path $shaderSource 'NrSpatialResolvePS.hlsl'
        $text=[IO.File]::ReadAllText($path).Replace('float3 residual = Decode(answerRaw) - guide;', 'float3 residual = float3(0, 0, 0);')
    } elseif($ShaderControl -eq 'EncodedArea') {
        $path=Join-Path $shaderSource 'NrSpatialDownsampleCS.hlsl'
        $text=[IO.File]::ReadAllText($path).Replace('float4(Decode(n.rgb), n.a)', 'n').Replace('float4(Encode(mean.rgb), mean.a)', 'mean')
    } elseif($ShaderControl -eq 'NoTransition') {
        $path=Join-Path $shaderSource 'NrSpatialResolvePS.hlsl'
        $text=[IO.File]::ReadAllText($path).Replace('strength = smoothstep(0, float(transitionWidth), min(edge.x, edge.y));', 'strength = 1;')
    } elseif($ShaderControl -eq 'NoShadow') {
        $path=Join-Path $shaderSource 'NrSpatialResolvePS.hlsl'
        $text=[IO.File]::ReadAllText($path).Replace("`r`n","`n").Replace('    if (all(raw.rgb == 0))'+"`n"+'        return raw;'+"`n", '').Replace('residual = ProtectNrResidual(nativeLinear, guide, residual);', '')
    } elseif($ShaderControl -eq 'NoNegativeShared') {
        $path=Join-Path $shaderSource 'NrSpatialCommon.hlsli'
        $text=[IO.File]::ReadAllText($path).Replace('max(min(residual.r, 0), max(min(residual.g, 0), min(residual.b, 0)))', '0.0')
    } elseif($ShaderControl -eq 'NoChromaToe' -or $ShaderControl -eq 'SourceOnly') {
        $path=Join-Path $shaderSource 'NrSpatialCommon.hlsli'
        $text=[IO.File]::ReadAllText($path)
        if($ShaderControl -eq 'NoChromaToe') { $text=$text.Replace('float chromaWeight = smoothstep(0, 1.0 / 128.0, level);', 'float chromaWeight = lumaWeight;') }
        else { $text=$text.Replace('float level = max(0, min(sourceLevel, guideLevel));', 'float level = max(0, sourceLevel);') }
    } else {
        $path=Join-Path $shaderSource 'NrSpatialDownsampleCS.hlsl'
        $text=[IO.File]::ReadAllText($path).Replace('p = min(uint2(nativePoint * float2(depthRect.zw) / float2(sourceRect.zw)), depthRect.zw - 1);',
            'p = min(uint2((float2(id.xy) + 0.5) * float2(depthRect.zw) / float2(workExtent)), depthRect.zw - 1);')
    }
    if($text -eq [IO.File]::ReadAllText($path)) { throw 'Shader negative control did not change its target.' }
    [IO.File]::WriteAllText($path,$text)
}
foreach($shader in @(@('NrSpatialDownsampleCS','cs_5_0'),@('NrSpatialResolvePS','ps_5_0'),@('NrNativeResolvePS','ps_5_0'),@('FullScreenQuadVS','vs_5_0'))) {
    & $fxc /nologo /Ges /WX /T $shader[1] /E main /Vn "g_$($shader[0])" /Fh "$OutputDirectory\$($shader[0]).h" "$shaderSource\$($shader[0]).hlsl"
    if($LASTEXITCODE) { throw "Shader compilation failed: $($shader[0])" }
}
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /MT /fp:precise /DNOMINMAX /DNR_SPATIAL_TEST_OBSERVER "/I$OutputDirectory" "$PSScriptRoot\nr_spatial_shader_regression.cpp" "$source\NrSpatialPipeline.cpp" "/Fe:$OutputDirectory\nr_spatial_shader_regression.exe" "/Fo:$OutputDirectory\\" /link d3d12.lib dxgi.lib d3dcompiler.lib
if($LASTEXITCODE) { throw "GPU fixture compilation failed: $LASTEXITCODE" }
if($Hardware) { & "$OutputDirectory\nr_spatial_shader_regression.exe" --hardware }
else { & "$OutputDirectory\nr_spatial_shader_regression.exe" }
if($LASTEXITCODE) { throw "GPU regression failed: $LASTEXITCODE" }
