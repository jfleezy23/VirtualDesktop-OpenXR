@echo off
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist bin\tests mkdir bin\tests
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\OpenXR-SDK\include" tests\nr_openxr_regression.cpp /Fe:bin\tests\nr_openxr_regression.exe /Fo:bin\tests\nr_openxr_regression.obj /link d3d11.lib dxgi.lib
exit /b %errorlevel%
