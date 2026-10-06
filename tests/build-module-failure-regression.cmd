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
if not exist bin\tests mkdir bin\tests
if not exist bin\tests\module-missing mkdir bin\tests\module-missing
if not exist bin\tests\module-shutdown mkdir bin\tests\module-shutdown
if not exist bin\tests\module-partial mkdir bin\tests\module-partial
if not exist bin\tests\module-import mkdir bin\tests\module-import
cl.exe /nologo /std:c++17 /EHsc /W4 /LD /DNR_TEST_MISSING_EVALUATE /I"external\DLSS\include" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_test_module.cpp /Fe:bin\tests\module-missing\nvngx_dlssnr.dll /Fo:bin\tests\module-missing\nr_test_module.obj /link /IMPLIB:bin\tests\module-missing\nr_test_module.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /LD /DNR_TEST_FAIL_SHUTDOWN_ONCE /I"external\DLSS\include" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_test_module.cpp /Fe:bin\tests\module-shutdown\nvngx_dlssnr.dll /Fo:bin\tests\module-shutdown\nr_test_module.obj /link /IMPLIB:bin\tests\module-shutdown\nr_test_module.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /LD /DNR_TEST_PARTIAL_CREATE /I"external\DLSS\include" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_test_module.cpp /Fe:bin\tests\module-partial\nvngx_dlssnr.dll /Fo:bin\tests\module-partial\nr_test_module.obj /link /IMPLIB:bin\tests\module-partial\nr_test_module.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /LD /DNR_TEST_FOREIGN_IMPORT /I"external\DLSS\include" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_test_module.cpp /Fe:bin\tests\module-import\nvngx_dlssnr.dll /Fo:bin\tests\module-import\nr_test_module.obj /link /IMPLIB:bin\tests\module-import\nr_test_module.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_module_failure_regression.cpp /Fe:bin\tests\nr_module_failure_regression.exe /Fo:bin\tests\nr_module_failure_regression.obj /link d3d11.lib dxgi.lib advapi32.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" tests\runtime_path_regression.cpp /Fe:bin\tests\runtime_path_regression.exe /Fo:bin\tests\runtime_path_regression.obj
exit /b %errorlevel%
