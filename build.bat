@echo off
setlocal
where zig >nul 2>nul || (echo [error] zig not found in PATH & exit /b 1)
echo [build] zig c++ src\main.cpp src\inject.cpp -O2 ...
zig c++ src\main.cpp src\inject.cpp -O2 -lws2_32 -luser32 -lkernel32 -o put_text.exe
if %errorlevel%==0 (echo [ok] built: put_text.exe) else (echo [error] build failed & exit /b 1)