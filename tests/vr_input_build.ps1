param(
    [string]$SourceDirectory = '',
    [string]$RuntimeObjectDirectory = '',
    [switch]$RecompileInputSources,
    [string]$TestSource = 'vr_input_regression.cpp',
    [switch]$BaselineWatcher,
    [switch]$UseExistingInputObjects
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$SourceDirectory) { $SourceDirectory = Join-Path $repo 'virtualdesktop-openxr' }
if (!$RuntimeObjectDirectory) { $RuntimeObjectDirectory = Join-Path $repo 'bin/code-analysis/obj' }
$output = Join-Path $repo 'bin/vr-input-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$includeDirectories = @(
    $SourceDirectory, $RuntimeObjectDirectory, (Join-Path $repo 'RuntimeConfiguration'),
    (Join-Path $repo 'external/OpenXR-SDK/include'),
    (Join-Path $repo 'external/OpenXR-SDK/src/common'),
    (Join-Path $repo 'external/OpenXR-MixedReality/Shared'),
    (Join-Path $repo 'external/OpenXR-MixedReality/Shared/XrUtility'),
    (Join-Path $repo 'external/OpenXR-MixedReality/Shared/SampleShared'),
    (Join-Path $repo 'external/LibOVR/Include'), (Join-Path $repo 'external/LibOVR/Include/Extras'),
    (Join-Path $repo 'external/LibOVR/Shim'), (Join-Path $repo 'external/Vulkan-SDK/include'),
    (Join-Path $repo 'external/OpenGL'), (Join-Path $repo 'external/fmt/include'),
    (Join-Path $repo 'external/openvr/headers'),
    (Join-Path $repo 'external/openvr/samples/drivers/drivers/handskeletonsimulation/src'),
    (Join-Path $repo 'external/openvr/samples/drivers/utils/vrmath'),
    (Join-Path $repo 'external/FidelityFX-FSR/ffx-fsr'),
    (Join-Path $repo 'external/FidelityFX-CAS/ffx-cas'), (Join-Path $repo 'external/cJSON'),
    (Join-Path $repo 'external/d3dx12'), (Join-Path $repo 'external/DLSS/include'),
    (Join-Path $repo 'packages/Detours.4.0.1/lib/native/include'),
    (Join-Path $repo 'packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/include'),
    (Join-Path $repo 'packages/Microsoft.GameInput.2.2.26100.6114/native/include')
)
$compileOptions = @('/nologo', '/std:c++17', '/EHsc', '/W3', '/MT', '/O2', '/Gy', '/c',
    '/D', 'RUNTIME_NAMESPACE=virtualdesktop_openxr', '/D', 'NDEBUG', '/D', 'UNICODE',
    '/D', '_UNICODE', '/D', '_CRT_SECURE_NO_WARNINGS', '/D', 'USING_GAMEINPUT')
foreach ($directory in $includeDirectories) { $compileOptions += "/I$directory" }
if ($BaselineWatcher) { $compileOptions += '/DVDXR_WATCHER_BASELINE' }
$testName = [IO.Path]::GetFileNameWithoutExtension($TestSource)
$testObject = Join-Path $output "$testName.obj"
& cl.exe @compileOptions (Join-Path $PSScriptRoot $TestSource) "/Fo$testObject"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$objects = @(Get-ChildItem -LiteralPath $RuntimeObjectDirectory -Filter '*.obj' | ForEach-Object FullName)
if (!$objects) { throw 'Build the ReleaseBundle runtime before linking this regression.' }
if ($RecompileInputSources) {
    foreach ($name in @('action', 'hand_tracking', 'instance', 'session')) {
        $newObject = Join-Path $output "vr_input_$name.obj"
        & cl.exe @compileOptions (Join-Path $SourceDirectory "$name.cpp") "/Fo$newObject"
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
        $objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne "$name.obj" })
        $objects += $newObject
    }
}
if ($UseExistingInputObjects) {
    foreach ($name in @('action', 'hand_tracking', 'instance', 'session')) {
        $objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne "$name.obj" })
        $objects += Join-Path $output "vr_input_$name.obj"
    }
}
$libraries = @('dxgi.lib', 'dxguid.lib', 'd3d11.lib', 'd3d12.lib', 'opengl32.lib', 'ntdll.lib',
    'kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib', 'comdlg32.lib', 'advapi32.lib',
    'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'odbc32.lib', 'odbccp32.lib',
    (Join-Path $repo 'packages/Detours.4.0.1/lib/native/libs/x64/detours.lib'),
    (Join-Path $repo 'packages/Microsoft.GameInput.2.2.26100.6114/native/lib/x64/GameInput.lib'),
    (Join-Path $repo 'external/Vulkan-SDK/lib/vulkan-1.lib'),
    (Join-Path $repo 'external/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib'))
& link.exe /nologo /LTCG /SUBSYSTEM:CONSOLE "/OUT:$output/$testName.exe" $testObject @objects @libraries
exit $LASTEXITCODE
