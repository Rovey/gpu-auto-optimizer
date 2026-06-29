@echo off
REM Launch GPU Auto Optimizer using the project venv (no console window).
REM gpu_optimizer.py self-elevates via a UAC prompt for hardware control.
start "" "%~dp0.venv\Scripts\pythonw.exe" "%~dp0gpu_optimizer.py"
