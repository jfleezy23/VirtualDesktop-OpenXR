@echo off
setlocal
cd /d "%~dp0.."
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
)
if not exist bin\tests\ovr-bridge mkdir bin\tests\ovr-bridge
cl.exe /nologo /std:c++17 /EHsc /W4 /LD tests\runtime_ovr_bridge_test_module.cpp /Fe:bin\tests\ovr-bridge\LibReviveXR64.dll /Fo:bin\tests\ovr-bridge\marker.obj /link /IMPLIB:bin\tests\ovr-bridge\marker.lib
if errorlevel 1 exit /b %errorlevel%
copy /y bin\tests\ovr-bridge\LibReviveXR64.dll bin\tests\ovr-bridge\OVRPlugin.dll >nul
if errorlevel 1 exit /b %errorlevel%
copy /y bin\tests\ovr-bridge\LibReviveXR64.dll bin\tests\ovr-bridge\VirtualDesktop.Injector64.dll >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" /I"packages\Detours.4.0.1\lib\native\include" tests\runtime_ovr_bridge_regression.cpp /Fe:bin\tests\ovr-bridge\runtime_ovr_bridge_regression.exe /Fo:bin\tests\ovr-bridge\regression.obj /link advapi32.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
exit /b %errorlevel%
