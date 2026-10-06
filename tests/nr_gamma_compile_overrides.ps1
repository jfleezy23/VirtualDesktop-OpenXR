param([string]$SourceDirectory,[string]$RuntimeObjectDirectory,[string]$OverrideDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$relativeIncludes=@('RuntimeConfiguration','external/OpenXR-SDK/include','external/OpenXR-SDK/src/common',
    'external/OpenXR-MixedReality/Shared','external/OpenXR-MixedReality/Shared/XrUtility',
    'external/OpenXR-MixedReality/Shared/SampleShared','external/LibOVR/Include','external/LibOVR/Include/Extras',
    'external/LibOVR/Shim','external/Vulkan-SDK/include','external/OpenGL','external/fmt/include','external/openvr/headers',
    'external/openvr/samples/drivers/drivers/handskeletonsimulation/src','external/openvr/samples/drivers/utils/vrmath',
    'external/FidelityFX-FSR/ffx-fsr','external/FidelityFX-CAS/ffx-cas','external/cJSON','external/d3dx12','external/DLSS/include',
    'packages/Detours.4.0.1/lib/native/include','packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/include',
    'packages/Microsoft.GameInput.2.2.26100.6114/native/include')
$options=@('/nologo','/std:c++17','/EHsc','/W3','/MT','/O2','/Gy','/c','/DRUNTIME_NAMESPACE=virtualdesktop_openxr',
    '/DNDEBUG','/DUNICODE','/D_UNICODE','/D_CRT_SECURE_NO_WARNINGS','/DUSING_GAMEINPUT',"/I$SourceDirectory","/I$RuntimeObjectDirectory")
foreach($path in $relativeIncludes){$options+="/I$(Join-Path $repo $path)"}
foreach($name in 'precompositor','d3d11_native'){
    & cl.exe @options (Join-Path $OverrideDirectory "$name.cpp") "/Fo$(Join-Path $RuntimeObjectDirectory "$name.obj")"
    if($LASTEXITCODE){exit $LASTEXITCODE}
}
