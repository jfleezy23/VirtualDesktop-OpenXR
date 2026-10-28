param([Parameter(Mandatory=$true)][string]$OutputDirectory,
      [Parameter(Mandatory=$true)][string]$VCToolsDirectory,
      [string]$WindowsSdkDirectory = "${env:ProgramFiles(x86)}/Windows Kits/10")
$ErrorActionPreference = 'Stop'
$sdkVersion = '10.0.26100.0'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$compiler = Join-Path $VCToolsDirectory 'bin/Hostx64/x64/cl.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'Supply an installed x64 MSVC toolset directory.' }
$env:INCLUDE = "$VCToolsDirectory/include;$WindowsSdkDirectory/Include/$sdkVersion/ucrt;$WindowsSdkDirectory/Include/$sdkVersion/um;$WindowsSdkDirectory/Include/$sdkVersion/shared"
$env:LIB = "$VCToolsDirectory/lib/x64;$WindowsSdkDirectory/Lib/$sdkVersion/ucrt/x64;$WindowsSdkDirectory/Lib/$sdkVersion/um/x64"
& $compiler /nologo /std:c++17 /EHsc /W4 /WX /MT "$PSScriptRoot/game_profiles_regression.cpp" "/Fo:$OutputDirectory/game_profiles.obj" "/Fe:$OutputDirectory/game_profiles.exe" /link advapi32.lib
if ($LASTEXITCODE) { throw "Profile regression compilation failed: $LASTEXITCODE" }
& "$OutputDirectory/game_profiles.exe"
if ($LASTEXITCODE) { throw "Profile policy regression failed: $LASTEXITCODE" }
