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
set "testVariant=%~1"
if not defined testVariant set "testVariant=current"
if not "%testVariant%"=="baseline" if not "%testVariant%"=="patched" if not "%testVariant%"=="current" exit /b 2
if not exist bin\tests\ovrnull-%testVariant%\objects mkdir bin\tests\ovrnull-%testVariant%\objects
rem Freeze the actual production objects; the baseline executable must remain immutable after the fix.
copy /y obj\x64\Release\OVRNull\driver.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\log.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\dllmain.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\pch.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\api.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\stubs.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\OVR_CAPI_Util.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
copy /y obj\x64\Release\OVRNull\OVR_StereoProjection.obj bin\tests\ovrnull-%testVariant%\objects\ >nul
cl.exe /nologo /std:c++20 /EHsc /W4 /MT /I"external\LibOVR\Include" /I"external\LibOVR\Include\Extras" /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.240803.1\include" tests\ovr_null_index_regression.cpp /Fe:bin\tests\ovrnull-%testVariant%\ovr_null_index_regression.exe /Fo:bin\tests\ovrnull-%testVariant%\ovr_null_index_regression.obj /link /LTCG /OPT:REF bin\tests\ovrnull-%testVariant%\objects\driver.obj bin\tests\ovrnull-%testVariant%\objects\log.obj bin\tests\ovrnull-%testVariant%\objects\dllmain.obj bin\tests\ovrnull-%testVariant%\objects\pch.obj bin\tests\ovrnull-%testVariant%\objects\api.obj bin\tests\ovrnull-%testVariant%\objects\stubs.obj bin\tests\ovrnull-%testVariant%\objects\OVR_CAPI_Util.obj bin\tests\ovrnull-%testVariant%\objects\OVR_StereoProjection.obj RuntimeObject.lib dxgi.lib dxguid.lib d3d11.lib winmm.lib user32.lib advapi32.lib ole32.lib oleaut32.lib
exit /b %errorlevel%
