param(
    [ValidateSet('v143', 'v145')][string]$Toolset = 'v145',
    [ValidateSet('Release', 'ReleaseBundle')][string]$Configuration = 'ReleaseBundle',
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$MsBuildPath = '',
    [switch]$FgOnly,
    [switch]$PrivateRuntime
)
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a fresh output directory for this build cohort.' }
$generator = if ($Toolset -eq 'v143') { 'Visual Studio 17 2022' } else { 'Visual Studio 18 2026' }
$compilerVersion = if ($Toolset -eq 'v143') { '14.44.35207' } else { '14.51.36231' }
$toolsetArgument = "$Toolset,version=$compilerVersion"
$deps = Join-Path $output 'deps'
& python (Join-Path $PSScriptRoot 'acquire-fg-source.py') --output $deps
if ($LASTEXITCODE) { throw 'Pinned source acquisition failed.' }
$ffxSource = Join-Path $deps 'fidelityfx/sdk'
$ffxBuild = Join-Path $output 'ffx-build'
& cmake -S $ffxSource -B $ffxBuild -G $generator -A x64 -T $toolsetArgument -DCMAKE_SYSTEM_VERSION=10.0.26100.0 `
    -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_API_BACKEND=DX12_X64 -DFFX_BUILD_AS_DLL=OFF
if ($LASTEXITCODE) { throw 'FidelityFX configure failed.' }
& cmake --build $ffxBuild --config Release --parallel 4
if ($LASTEXITCODE) { throw 'FidelityFX build failed.' }
$moduleSource = Join-Path $source 'nr-frame-generation'
$moduleBuild = Join-Path $output 'module'
$provenance = Join-Path $PSScriptRoot 'fg-build-provenance.py'
$moduleReceipt = Join-Path $moduleBuild 'build-source.json'
& python $provenance --repo $source --kind module --receipt $moduleReceipt --capture
if ($LASTEXITCODE) { throw 'Cannot capture module build inputs.' }
& cmake -S $moduleSource -B $moduleBuild -G $generator -A x64 -T $toolsetArgument `
    -DCMAKE_SYSTEM_VERSION=10.0.26100.0 "-DFG_DEPS=$deps"
if ($LASTEXITCODE) { throw 'Frame generation configure failed.' }
& cmake --build $moduleBuild --config Release --parallel 4
if ($LASTEXITCODE) { throw 'Frame generation build failed.' }
& python $provenance --repo $source --kind module --receipt $moduleReceipt --artifact-directory (Join-Path $moduleBuild 'Release')
if ($LASTEXITCODE) { throw 'Module build inputs or artifacts changed.' }
if (!$FgOnly) {
    # Runtime configuration remains distinct from the C++20 synthesis module.
    $msbuild = if ($MsBuildPath) { $MsBuildPath } elseif ($Toolset -eq 'v143') {
        Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe'
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $found = @(& $vswhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe')
        if ($LASTEXITCODE -ne 0 -or !$found) { throw 'Visual Studio 2026 C++ tools were not found; supply -MsBuildPath.' }
        $found[0]
    }
    if (!(Test-Path -LiteralPath $msbuild)) {
        throw 'MSBuild was not found. Supply -MsBuildPath for your Visual Studio installation.'
    }
    $runtimeOutput = Join-Path $output 'runtime'
    $runtimeReceipt = Join-Path $runtimeOutput 'build-source.json'
    & python $provenance --repo $source --kind runtime --receipt $runtimeReceipt --capture
    if ($LASTEXITCODE) { throw 'Cannot capture runtime build inputs.' }
    $priorCl = $env:CL
    $priorUnderscoreCl = ${env:_CL_}
    try {
        Remove-Item Env:CL, Env:_CL_ -ErrorAction SilentlyContinue
        if ($PrivateRuntime) { ${env:_CL_} = '/DLOAD_LOCAL_LIBOVR' }
        if ($Toolset -eq 'v145') {
            $objects = Join-Path $runtimeOutput 'obj'
            New-Item -ItemType Directory -Force -Path $objects | Out-Null
            $commit = & git -C $source rev-parse --verify HEAD
            if ($LASTEXITCODE -or $commit -notmatch '^[a-f0-9]{40}$') { throw 'Cannot determine source revision.' }
            [IO.File]::WriteAllText((Join-Path $objects 'commit.h'), "const char* RuntimeCommitHash = `"$commit`";`n", [Text.Encoding]::ASCII)
            $arguments = @(
                (Join-Path $source 'virtualdesktop-openxr/virtualdesktop-openxr.vcxproj'), '/t:Rebuild', '/m:2', '/nologo', '/v:minimal',
                "/p:Configuration=$Configuration", '/p:Platform=x64', '/p:PlatformToolset=v145',
                '/p:VCToolsVersion=14.51.36231', '/p:WindowsTargetPlatformVersion=10.0.26100.0',
                ("/p:SolutionDir=" + $source.Replace('\', '/') + '/'),
                ("/p:OutDir=" + $runtimeOutput.Replace('\', '/') + '/'),
                ("/p:IntDir=" + $objects.Replace('\', '/') + '/'),
                ("/p:ForceImportBeforeCppTargets=" + (Join-Path $PSScriptRoot 'Experimental-MSVC145.props')),
                '/p:PreferredToolArchitecture=x64', '/p:PreBuildEventUseInBuild=false', '/p:PostBuildEventUseInBuild=false'
            )
            & $msbuild @arguments
        } else {
            & (Join-Path $PSScriptRoot 'Build-CommunityRuntime.ps1') -Configuration $Configuration -Platform x64 `
                -SourceDirectory $source -OutputDirectory (Join-Path $output 'runtime') -MsBuildPath $msbuild -Toolset $Toolset -VCToolsVersion $compilerVersion
        }
        $compilerExit = $LASTEXITCODE
    } finally {
        $env:CL = $priorCl
        ${env:_CL_} = $priorUnderscoreCl
    }
    if ($compilerExit) { throw 'Runtime build failed.' }
    & python $provenance --repo $source --kind runtime --receipt $runtimeReceipt --artifact-directory $runtimeOutput
    if ($LASTEXITCODE) { throw 'Runtime build inputs or artifacts changed.' }
    @{private_backend=[bool]$PrivateRuntime;exit_code=0;compiler_version=$compilerVersion;
      runtime_sha256=(Get-FileHash -LiteralPath (Join-Path $runtimeOutput 'virtualdesktop-openxr.dll')).Hash.ToLowerInvariant()} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runtimeOutput 'result.json') -Encoding ASCII
}
Write-Output "PASS: frame generation $Toolset module; $moduleBuild/Release/vdxr-frame-generation.dll"
