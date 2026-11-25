$ErrorActionPreference = 'Stop'
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer'
$installation = & (Join-Path $installer 'vswhere.exe') -latest -products '*' -version '[18.0,19.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'Visual Studio 2026 with C++ tools was not found.' }

# The hosted image can advance its default compiler while 14.51 remains an optional component.
$compiler = Join-Path $installation 'VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe'
if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) {
    $component = 'Microsoft.VisualStudio.Component.VC.14.51.x86.x64'
    $setup = Start-Process -FilePath (Join-Path $installer 'setup.exe') -WindowStyle Hidden -Wait -PassThru `
        -ArgumentList "modify --installPath `"$installation`" --add $component --quiet --norestart"
    if ($setup.ExitCode -notin 0, 3010) { throw "Visual Studio Installer exited with $($setup.ExitCode)." }
}
$installedCompiler = Get-Item -LiteralPath $compiler
Write-Host "Pinned compiler: $($installedCompiler.FullName) (servicing version $($installedCompiler.VersionInfo.ProductVersion))"
return $installation
