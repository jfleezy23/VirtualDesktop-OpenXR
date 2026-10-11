@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist bin\tests mkdir bin\tests
cl.exe /nologo /std:c++17 /EHsc /W4 /MT /I"external\d3dx12" /I"packages\Detours.4.0.1\lib\native\include" tests\nr_command_context_regression.cpp /Fe:bin\tests\nr_command_context_regression.exe /Fo:bin\tests\nr_command_context_regression.obj /link d3d12.lib dxgi.lib "packages\Detours.4.0.1\lib\native\libs\x64\detours.lib"
exit /b %errorlevel%
