@echo off
REM One-time setup: create the project venv and install all dependencies
REM (including the CUDA-12 GPU stress backend). Requires Python 3.12 (py -3.12).
echo Creating virtual environment (.venv)...
py -3.12 -m venv "%~dp0.venv"
if errorlevel 1 (
  echo ERROR: could not create venv. Is Python 3.12 installed? ^(py -3.12^)
  pause & exit /b 1
)
echo Upgrading pip...
"%~dp0.venv\Scripts\python.exe" -m pip install --upgrade pip
echo Installing dependencies ^(this downloads the CUDA runtime, ~1 GB^)...
"%~dp0.venv\Scripts\python.exe" -m pip install -r "%~dp0requirements.txt"
if errorlevel 1 (
  echo ERROR: dependency install failed.
  pause & exit /b 1
)
echo.
echo Setup complete. Launch the app with start.bat
pause
