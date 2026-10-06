@echo off
setlocal
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vr_input_build.ps1" %*
exit /b %errorlevel%
