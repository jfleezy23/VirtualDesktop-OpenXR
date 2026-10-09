@echo off
setlocal
cd /d "%~dp0.."
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
)
if not exist bin\tests\redirect mkdir bin\tests\redirect
cl.exe /nologo /std:c++17 /EHsc /W4 /LD /I"external\OpenXR-SDK\include" tests\runtime_redirect_test_module.cpp /Fe:bin\tests\redirect\vdxr_redirect_failure_test.dll /Fo:bin\tests\redirect\failure.obj /link /IMPLIB:bin\tests\redirect\failure.lib
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /LD /DREDIRECT_TEST_SUCCESS /I"external\OpenXR-SDK\include" tests\runtime_redirect_test_module.cpp /Fe:bin\tests\redirect\vdxr_redirect_success_test.dll /Fo:bin\tests\redirect\success.obj /link /IMPLIB:bin\tests\redirect\success.lib
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" /I"packages\Detours.4.0.1\lib\native\include" tests\runtime_redirect_regression.cpp /Fe:bin\tests\redirect\runtime_redirect_regression.exe /Fo:bin\tests\redirect\runtime_redirect_regression.obj /link advapi32.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" /I"packages\Detours.4.0.1\lib\native\include" tests\runtime_negotiation_regression.cpp /Fe:bin\tests\redirect\runtime_negotiation_regression.exe /Fo:bin\tests\redirect\runtime_negotiation_regression.obj /link advapi32.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
exit /b %errorlevel%
