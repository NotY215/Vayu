@echo off
setlocal
cd /d "%~dp0"

set "COMPILER=build\x64-debug\bin\vayuc.exe"
set "ICON=assets\Vayu_logo.ico"
set "ICON_SCRIPT=tools\apply_vayu_icon.ps1"

if not exist "%COMPILER%" (
    echo ERROR: Vayu compiler not found: %COMPILER%
    echo Build the compiler first.
    exit /b 1
)
if not exist "%ICON%" (
    echo ERROR: Vayu executable icon not found: %ICON%
    exit /b 1
)
if not exist "%ICON_SCRIPT%" (
    echo ERROR: Icon resource helper not found: %ICON_SCRIPT%
    exit /b 1
)

call "%COMPILER%" vayu-src\vayu.vyu --native-out vayu.exe
if errorlevel 1 (
    echo ERROR: Native compilation of vayu.exe failed. No icon was applied.
    exit /b 1
)
if not exist "vayu.exe" (
    echo ERROR: Compiler reported success but vayu.exe was not created.
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ICON_SCRIPT%" -Executable "vayu.exe" -Icon "%ICON%"
if errorlevel 1 (
    echo ERROR: Could not apply assets\Vayu_logo.ico to vayu.exe.
    exit /b 1
)

call "%COMPILER%" vayu-src\vcode.vyu --native-out vcode.exe
if errorlevel 1 exit /b 1
call "%COMPILER%" vayu-src\vlex.vyu --native-out vlex.exe
if errorlevel 1 exit /b 1
call "%COMPILER%" vayu-src\vparse.vyu --native-out vparse.exe
if errorlevel 1 exit /b 1

echo.
echo Build commands completed.
exit /b 0
