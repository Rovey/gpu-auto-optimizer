#!/usr/bin/env python3
"""GPU Optimizer Installer — sets up the application in %LOCALAPPDATA%."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    print("=" * 60)
    print("  GPU Optimizer — Installer")
    print("=" * 60)
    print()

    # 1. Check Python version
    if sys.version_info < (3, 8):
        print(f"ERROR: Python 3.8+ required (found {sys.version})")
        sys.exit(1)
    print(f"[OK] Python {sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")

    # 2. Check nvidia-smi
    try:
        r = subprocess.run(["nvidia-smi", "-L"], capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            print("ERROR: nvidia-smi not working. Install NVIDIA drivers.")
            sys.exit(1)
        print(f"[OK] nvidia-smi found")
        for line in r.stdout.strip().splitlines():
            print(f"     {line}")
    except FileNotFoundError:
        print("ERROR: nvidia-smi not found. Install NVIDIA drivers.")
        sys.exit(1)

    # 3. Detect CUDA version
    cuda_major = _detect_cuda_version()
    print(f"[OK] CUDA version: {cuda_major}.x" if cuda_major else "[WARN] Could not detect CUDA version")

    # 4. Set up install directory
    local_app = os.environ.get("LOCALAPPDATA", "")
    if not local_app:
        print("ERROR: %LOCALAPPDATA% not set")
        sys.exit(1)

    install_dir = Path(local_app) / "GPUOptimizer"
    app_dir = install_dir / "app"
    venv_dir = install_dir / "venv"
    config_dir = install_dir / "config"
    logs_dir = install_dir / "logs"

    print(f"\nInstall location: {install_dir}")

    for d in [app_dir, config_dir, logs_dir]:
        d.mkdir(parents=True, exist_ok=True)
    print("[OK] Directory structure created")

    # 5. Copy source files
    src_dir = Path(__file__).parent

    # Copy src/ package
    dest_src = app_dir / "src"
    if dest_src.exists():
        shutil.rmtree(dest_src)
    shutil.copytree(src_dir / "src", dest_src, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    # Copy entry point and requirements
    for f in ["gpu_optimizer.py", "requirements.txt"]:
        src_file = src_dir / f
        if src_file.exists():
            shutil.copy2(src_file, app_dir / f)
    print("[OK] Source files copied")

    # 6. Create venv
    if not venv_dir.exists():
        print("\nCreating virtual environment...")
        subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], check=True)

    pip_exe = venv_dir / "Scripts" / "pip.exe"
    python_exe = venv_dir / "Scripts" / "python.exe"
    pythonw_exe = venv_dir / "Scripts" / "pythonw.exe"
    print("[OK] Virtual environment ready")

    # 7. Install dependencies
    print("\nInstalling dependencies...")
    subprocess.run(
        [str(pip_exe), "install", "--upgrade", "pip"],
        capture_output=True,
    )
    subprocess.run(
        [str(pip_exe), "install", "-r", str(app_dir / "requirements.txt")],
        check=True,
    )
    print("[OK] Base dependencies installed")

    # 8. Install CuPy
    cupy_pkg = _cupy_package_for_cuda(cuda_major)
    if cupy_pkg:
        print(f"\nInstalling {cupy_pkg}...")
        result = subprocess.run(
            [str(pip_exe), "install", cupy_pkg],
            capture_output=True, text=True,
        )
        if result.returncode == 0:
            print(f"[OK] {cupy_pkg} installed")
        else:
            print(f"[WARN] {cupy_pkg} install failed. CuPy is required for stress testing.")
            print(f"       Try manually: {pip_exe} install {cupy_pkg}")
    else:
        print("[WARN] Could not determine CuPy package for your CUDA version.")
        print("       Install CuPy manually for stress testing to work.")

    # 9. Verify CuPy
    print("\nVerifying CuPy...")
    cupy_ok = subprocess.run(
        [str(python_exe), "-c", "import cupy; x = cupy.array([1,2,3]); print('CuPy OK:', cupy.__version__)"],
        capture_output=True, text=True,
    )
    if cupy_ok.returncode == 0:
        print(f"[OK] {cupy_ok.stdout.strip()}")
    else:
        print("[WARN] CuPy verification failed. Stress testing may not work.")

    # 10. Create shortcuts
    _create_shortcuts(pythonw_exe, app_dir / "gpu_optimizer.py", install_dir)

    # 11. Ask about auto-apply
    print("\nWould you like to enable auto-apply on boot? (y/n)")
    try:
        answer = input("> ").strip().lower()
        if answer in ("y", "yes"):
            boot_script = app_dir / "src" / "boot_apply.py"
            subprocess.run(
                [
                    "schtasks", "/create",
                    "/tn", "GPUOptimizer_BootApply",
                    "/tr", f'"{python_exe}" "{boot_script}"',
                    "/sc", "ONLOGON",
                    "/rl", "HIGHEST",
                    "/f",
                ],
                capture_output=True,
            )
            print("[OK] Boot-apply task registered")
    except (EOFError, KeyboardInterrupt):
        print("\nSkipped auto-apply setup")

    # Summary
    print("\n" + "=" * 60)
    print("  Installation complete!")
    print("=" * 60)
    print(f"\n  Install location: {install_dir}")
    print(f"  Launch: Double-click desktop shortcut or Start Menu entry")
    print(f"  Or run: {pythonw_exe} {app_dir / 'gpu_optimizer.py'}")
    print()


def _detect_cuda_version() -> str | None:
    """Detect CUDA major version from nvidia-smi."""
    try:
        r = subprocess.run(
            ["nvidia-smi", "-q"], capture_output=True, text=True, timeout=15,
            encoding="utf-8", errors="ignore",
        )
        for line in r.stdout.splitlines():
            if "CUDA Version" in line:
                val = line.split(":", 1)[-1].strip()
                major = val.split(".", 1)[0]
                if major.isdigit():
                    return major
    except Exception:
        pass
    return None


def _cupy_package_for_cuda(cuda_major: str | None) -> str | None:
    """Return the correct CuPy pip package name for the CUDA version."""
    if cuda_major == "13":
        return "cupy-cuda13x"
    elif cuda_major == "12":
        return "cupy-cuda12x"
    elif cuda_major == "11":
        return "cupy-cuda11x"
    return None


def _create_shortcuts(pythonw_exe: Path, script: Path, install_dir: Path) -> None:
    """Create desktop and Start Menu shortcuts."""
    try:
        import win32com.client
    except ImportError:
        print("[WARN] pywin32 not available — skipping shortcut creation")
        print(f"       Run manually: {pythonw_exe} {script}")
        return

    shell = win32com.client.Dispatch("WScript.Shell")

    # Desktop shortcut
    try:
        desktop = Path(shell.SpecialFolders("Desktop"))
        lnk = desktop / "GPU Optimizer.lnk"
        shortcut = shell.CreateShortCut(str(lnk))
        shortcut.TargetPath = str(pythonw_exe)
        shortcut.Arguments = f'"{script}"'
        shortcut.WorkingDirectory = str(install_dir)
        shortcut.Description = "GPU Optimizer"
        shortcut.save()
        print(f"[OK] Desktop shortcut: {lnk}")
    except Exception as e:
        print(f"[WARN] Desktop shortcut failed: {e}")

    # Start Menu shortcut
    try:
        start_menu = Path(os.environ.get("APPDATA", "")) / "Microsoft" / "Windows" / "Start Menu" / "Programs"
        lnk = start_menu / "GPU Optimizer.lnk"
        shortcut = shell.CreateShortCut(str(lnk))
        shortcut.TargetPath = str(pythonw_exe)
        shortcut.Arguments = f'"{script}"'
        shortcut.WorkingDirectory = str(install_dir)
        shortcut.Description = "GPU Optimizer"
        shortcut.save()
        print(f"[OK] Start Menu shortcut: {lnk}")
    except Exception as e:
        print(f"[WARN] Start Menu shortcut failed: {e}")


if __name__ == "__main__":
    main()
