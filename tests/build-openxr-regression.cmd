@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
set "testOutput=bin\tests"
if not "%~1"=="" set "testOutput=%~1"
if not exist "%testOutput%" mkdir "%testOutput%"
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" tests\nr_openxr_regression.cpp /Fe:"%testOutput%\nr_openxr_regression.exe" /Fo:"%testOutput%\nr_openxr_regression.obj" /link d3d11.lib dxgi.lib
exit /b %errorlevel%
