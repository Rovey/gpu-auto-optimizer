#!/usr/bin/env python3
"""GPU Optimizer -- launch the GUI application."""
import ctypes
import sys
from pathlib import Path


def _is_admin() -> bool:
    """Check if the current process has administrator privileges."""
    try:
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except (AttributeError, OSError):
        return False


def _request_admin_and_relaunch() -> None:
    """Re-launch this script with a UAC elevation prompt."""
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", sys.executable, f'"{__file__}"', None, 1
    )
    sys.exit(0)


# Ensure src/ is importable
sys.path.insert(0, str(Path(__file__).parent))


def main():
    if not _is_admin():
        _request_admin_and_relaunch()

    from src.gui.app import GPUOptimizerApp
    app = GPUOptimizerApp()
    app.run()


if __name__ == "__main__":
    main()
