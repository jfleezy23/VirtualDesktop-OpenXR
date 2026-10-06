@echo off
setlocal
cd /d "%~dp0.."
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  if defined VSINSTALLDIR (
    call "%VSINSTALLDIR%Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
  ) else (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
  )
)
set "testOutput=bin\tests"
if not "%~1"=="" set "testOutput=%~1"
if not exist "%testOutput%" mkdir "%testOutput%"
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" /I"external\LibOVR\Include" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_pipeline_regression.cpp /Fe:"%testOutput%\nr_pipeline_regression.exe" /Fo:"%testOutput%\nr_pipeline_regression.obj" /link d3d11.lib dxgi.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
exit /b %errorlevel%
