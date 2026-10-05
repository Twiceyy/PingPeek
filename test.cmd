@echo off
rem Builds and runs PingPeek's tests. Set PINGPEEK_LIVE=1 to also ping the server of the match you're in.
setlocal
cd /d "%~dp0"
where zig >nul 2>&1 || (echo Zig not found. Install it with: winget install zig.zig & exit /b 1)
zig cc -target x86_64-windows-gnu -O2 -Wall -Wextra -o "%TEMP%\pingpeek_tests.exe" tests\test_pingpeek.c -luser32 -lgdi32 -lws2_32 -lshell32 || exit /b 1
"%TEMP%\pingpeek_tests.exe"
