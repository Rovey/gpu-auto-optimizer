@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if "%~1"=="" (
    echo [ERROR] Missing run label.
    echo Usage: run_with_log.bat ^<label^> [gpu_optimizer args]
    pause
    exit /b 2
)

set "ALL_ARGS=%*"
for /f "tokens=1*" %%a in ("%ALL_ARGS%") do (
    set "RUN_LABEL=%%a"
    set "PY_ARGS=%%b"
)

if not defined PY_ARGS (
    echo [ERROR] Missing gpu_optimizer arguments.
    echo Usage: run_with_log.bat ^<label^> [gpu_optimizer args]
    pause
    exit /b 2
)

set "LOG_DIR=%~dp0logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "RUN_TS=%%i"
set "LOG_FILE=%LOG_DIR%\%RUN_LABEL%_%RUN_TS%.log"

echo [INFO] Logging to: "%LOG_FILE%"
python -u run_with_log.py --log "%LOG_FILE%" -- !PY_ARGS!
set "EXIT_CODE=%ERRORLEVEL%"

echo [INFO] Log saved: "%LOG_FILE%"
if not "%EXIT_CODE%"=="0" (
    echo.
    echo [ERROR] Script failed. Press any key to close.
    pause >nul
)

exit /b %EXIT_CODE%
