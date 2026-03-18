@echo off
setlocal EnableExtensions EnableDelayedExpansion
title GPU Optimizer - Setup
echo ============================================
echo  GPU Auto Optimizer - Installation
echo ============================================
echo.

:: Check for Python
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found. Install Python 3.8+ from https://python.org
    pause
    exit /b 1
)

echo Installing required Python packages...
python -m pip install --upgrade pip
python -m pip uninstall -y pynvml >nul 2>&1
python -m pip install -r requirements.txt

echo.
echo ============================================
echo  Detecting CUDA version for CuPy install...
echo ============================================

set "CUDA_PKG="
set "CUDA_VER="
set "CUDA_VER_RAW="
for /f "tokens=2 delims=:" %%v in ('nvidia-smi -q 2^>nul ^| findstr /i /c:"CUDA Version"') do (
    set "CUDA_VER_RAW=%%v"
)

:: Trim leading spaces by taking first whitespace-delimited token
for /f "tokens=1" %%v in ("!CUDA_VER_RAW!") do (
    set "CUDA_VER=%%v"
)

if not defined CUDA_VER (
    echo [WARN] Could not detect CUDA version. Skipping CuPy install.
    goto :show_cupy_manual_help
)

echo Detected CUDA version: !CUDA_VER!

:: Extract major version (first char before the dot)
for /f "tokens=1 delims=." %%m in ("!CUDA_VER!") do set "CUDA_MAJOR=%%m"

if "!CUDA_MAJOR!"=="13" set "CUDA_PKG=cupy-cuda13x"
if "!CUDA_MAJOR!"=="12" set "CUDA_PKG=cupy-cuda12x"
if "!CUDA_MAJOR!"=="11" set "CUDA_PKG=cupy-cuda11x"
if "!CUDA_MAJOR!"=="10" set "CUDA_PKG=cupy-cuda102"

if not defined CUDA_PKG (
    echo [WARN] Unsupported CUDA major version: !CUDA_MAJOR!. Skipping CuPy install.
    goto :show_cupy_manual_help
)

echo Installing !CUDA_PKG!...
python -m pip install !CUDA_PKG!
if errorlevel 1 (
    echo [WARN] CuPy install failed. GPU stress test will fall back to FurMark/idle mode.
    goto :show_cupy_manual_help
) else (
    echo Installing NVIDIA runtime components for CuPy...
    python -m pip install nvidia-cuda-nvrtc >nul 2>&1
    python -m pip install nvidia-curand >nul 2>&1
    python -m pip install nvidia-cublas >nul 2>&1
    echo [OK] CuPy installed successfully.
)

goto :skip_cupy

:show_cupy_manual_help
echo ============================================
echo  Optional: CuPy GPU stress test support
echo  Install ONE of the following depending on
echo  your CUDA version ^(check: nvidia-smi^)
echo ============================================
echo   CUDA 13.x:  pip install cupy-cuda13x
echo   CUDA 12.x:  pip install cupy-cuda12x
echo   CUDA 11.x:  pip install cupy-cuda11x
echo.

:skip_cupy

:: Create shortcut batch files for convenience
echo @echo off > run_safe.bat
echo cd /d "%%~dp0" >> run_safe.bat
echo call run_with_log.bat safe --risk safe >> run_safe.bat

echo @echo off > run_balanced.bat
echo cd /d "%%~dp0" >> run_balanced.bat
echo call run_with_log.bat balanced --risk balanced >> run_balanced.bat

echo @echo off > run_performance.bat
echo cd /d "%%~dp0" >> run_performance.bat
echo call run_with_log.bat performance --risk performance >> run_performance.bat

echo @echo off > run_monitor.bat
echo cd /d "%%~dp0" >> run_monitor.bat
echo call run_with_log.bat monitor --monitor >> run_monitor.bat

echo @echo off > run_reset.bat
echo cd /d "%%~dp0" >> run_reset.bat
echo call run_with_log.bat reset --reset >> run_reset.bat

echo.
echo ============================================
echo  Installation complete!
echo  Shortcut scripts created:
echo    run_safe.bat        - SAFE mode
echo    run_balanced.bat    - BALANCED mode ^(recommended^)
echo    run_performance.bat - PERFORMANCE mode
echo    run_monitor.bat     - Live monitoring only
echo    run_reset.bat       - Reset all GPUs to stock
echo.
echo  TIP: Run as Administrator for full NVAPI
echo       power-limit and OC control.
echo ============================================
echo.
pause
exit /b
