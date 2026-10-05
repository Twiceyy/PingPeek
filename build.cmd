@echo off
rem Builds PingPeek.exe next to this script. Needs Zig, built and tested with 0.17.0:
rem   winget install zig.zig
setlocal
cd /d "%~dp0"
where zig >nul 2>&1 || (echo Zig not found. Install it with: winget install zig.zig & exit /b 1)
for /f %%v in ('zig version') do set ZIGV=%%v
if not "%ZIGV:~0,5%"=="0.17." echo note: PingPeek is built and tested with Zig 0.17, this is %ZIGV%
zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows -Wall -Wextra -o PingPeek.exe pingpeek.c pingpeek.rc -luser32 -lgdi32 -lws2_32 -lshell32
