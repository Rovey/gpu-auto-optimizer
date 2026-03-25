"""Windows Task Scheduler integration for boot-apply."""
from __future__ import annotations

import subprocess

TASK_NAME = "GPUOptimizer_BootApply"


def register_boot_task(python_exe: str, script_path: str) -> bool:
    """Create a Task Scheduler task that runs boot_apply.py at user logon."""
    # Delete existing task first (ignore errors if it doesn't exist)
    subprocess.run(
        ["schtasks", "/delete", "/tn", TASK_NAME, "/f"],
        capture_output=True,
    )
    result = subprocess.run(
        [
            "schtasks", "/create",
            "/tn", TASK_NAME,
            "/tr", f'"{python_exe}" "{script_path}"',
            "/sc", "ONLOGON",
            "/rl", "HIGHEST",
            "/f",
        ],
        capture_output=True,
    )
    return result.returncode == 0


def unregister_boot_task() -> bool:
    """Remove the boot-apply task from Task Scheduler."""
    result = subprocess.run(
        ["schtasks", "/delete", "/tn", TASK_NAME, "/f"],
        capture_output=True,
    )
    return result.returncode == 0


def is_task_registered() -> bool:
    """Check if the boot-apply task exists in Task Scheduler."""
    result = subprocess.run(
        ["schtasks", "/query", "/tn", TASK_NAME],
        capture_output=True,
    )
    return result.returncode == 0
