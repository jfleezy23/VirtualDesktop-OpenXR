@echo off
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist bin\tests mkdir bin\tests
cl.exe /nologo /std:c++17 /EHsc /W4 /I"external\d3dx12" tests\nr_fence_regression.cpp /Fe:bin\tests\nr_fence_regression.exe /Fo:bin\tests\nr_fence_regression.obj /link d3d12.lib dxgi.lib
if errorlevel 1 exit /b 2
bin\tests\nr_fence_regression.exe
