"""Settings screen -- auto-apply toggle, boot log viewer, reset, about."""
from __future__ import annotations

import sys
import tkinter as tk
from tkinter import ttk, messagebox
from typing import Optional

from .widgets import LogViewer


class SettingsScreen(ttk.Frame):
    """Settings and configuration screen."""

    def __init__(self, parent: tk.Widget, config, save_config_fn=None) -> None:
        super().__init__(parent)
        self._config = config
        self._save_config = save_config_fn
        self._build_ui()
        self.refresh()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        # Scrollable container
        canvas = tk.Canvas(self, highlightthickness=0)
        scrollbar = ttk.Scrollbar(self, orient="vertical", command=canvas.yview)
        self._inner = ttk.Frame(canvas)
        self._inner.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        canvas.create_window((0, 0), window=self._inner, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        # Section 1: Auto-Apply on Boot
        auto_frame = ttk.LabelFrame(self._inner, text="Auto-Apply on Boot")
        auto_frame.pack(fill="x", padx=10, pady=5)

        self._auto_apply_var = tk.BooleanVar(value=False)
        self._auto_check = ttk.Checkbutton(
            auto_frame,
            text="Enable auto-apply on login",
            variable=self._auto_apply_var,
            command=self._toggle_auto_apply,
        )
        self._auto_check.pack(anchor="w", padx=10, pady=5)

        self._scheduler_status = ttk.Label(auto_frame, text="")
        self._scheduler_status.pack(anchor="w", padx=10, pady=(0, 5))

        # Section 2: Boot-Apply Log
        log_frame = ttk.LabelFrame(self._inner, text="Boot-Apply Log")
        log_frame.pack(fill="x", padx=10, pady=5)

        self._boot_log = LogViewer(log_frame, max_lines=100)
        self._boot_log.pack(fill="x", padx=5, pady=5)

        ttk.Button(
            log_frame, text="Clear Log", command=self._clear_boot_log
        ).pack(anchor="e", padx=10, pady=5)

        # Section 3: GPU Controls
        gpu_frame = ttk.LabelFrame(self._inner, text="GPU Controls")
        gpu_frame.pack(fill="x", padx=10, pady=5)

        self._gpu_info_label = ttk.Label(gpu_frame, text="")
        self._gpu_info_label.pack(anchor="w", padx=10, pady=5)

        ttk.Button(
            gpu_frame, text="Reset to Stock", command=self._reset_to_stock
        ).pack(anchor="w", padx=10, pady=5)

        # Section 4: About
        about_frame = ttk.LabelFrame(self._inner, text="About")
        about_frame.pack(fill="x", padx=10, pady=5)

        self._about_text = ttk.Label(about_frame, text="", justify="left")
        self._about_text.pack(anchor="w", padx=10, pady=10)

    # ------------------------------------------------------------------
    # Refresh / reload
    # ------------------------------------------------------------------

    def refresh(self) -> None:
        """Reload config and update all sections."""
        cfg = self._config

        # Auto-apply checkbox
        self._auto_apply_var.set(cfg.auto_apply_on_boot)
        try:
            from ..scheduler import is_task_registered

            registered = is_task_registered()
            self._scheduler_status.configure(
                text=f"Task Scheduler: {'Registered' if registered else 'Not registered'}"
            )
        except Exception:
            self._scheduler_status.configure(text="Task Scheduler: Unknown")

        # Boot log entries
        self._boot_log.clear()
        for entry in cfg.boot_apply.boot_log:
            ts = entry.get("timestamp", "?")
            result = entry.get("result", "?")
            details = entry.get("details", "")
            self._boot_log.append(f"[{ts}] {result}: {details}")

        # GPU info
        gpu_uuid = cfg.boot_apply.gpu_uuid or "Unknown"
        driver = cfg.boot_apply.driver_version or "Unknown"
        self._gpu_info_label.configure(
            text=f"GPU UUID: {gpu_uuid}\nDriver: {driver}"
        )

        # About
        python_ver = (
            f"{sys.version_info.major}.{sys.version_info.minor}"
            f".{sys.version_info.micro}"
        )

        cupy_status = "Not installed"
        try:
            import cupy

            cupy_status = f"v{cupy.__version__}"
        except ImportError:
            pass

        nvapi_status = "Unknown"
        try:
            from ..backends.nvapi import NVAPIBackend

            nvapi_status = (
                "Available" if NVAPIBackend().is_available() else "Not available"
            )
        except Exception:
            nvapi_status = "Not available"

        self._about_text.configure(
            text=(
                f"GPU Optimizer v1.0\n"
                f"Python: {python_ver}\n"
                f"CuPy: {cupy_status}\n"
                f"NVAPI: {nvapi_status}"
            )
        )

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def _toggle_auto_apply(self) -> None:
        """Toggle auto-apply and register/unregister scheduler task."""
        enabled = self._auto_apply_var.get()
        self._config.auto_apply_on_boot = enabled

        try:
            if enabled:
                from ..scheduler import register_boot_task
                from ..config import get_app_dir
                import os

                python_exe = sys.executable
                script = os.path.join(get_app_dir(), "app", "src", "boot_apply.py")
                register_boot_task(python_exe, script)
            else:
                from ..scheduler import unregister_boot_task

                unregister_boot_task()
        except Exception as e:
            messagebox.showwarning(
                "Scheduler", f"Could not update Task Scheduler: {e}"
            )

        if self._save_config:
            self._save_config(self._config)
        self.refresh()

    def _clear_boot_log(self) -> None:
        """Clear boot log entries."""
        self._config.boot_apply.boot_log.clear()
        if self._save_config:
            self._save_config(self._config)
        self._boot_log.clear()

    def _reset_to_stock(self) -> None:
        """Reset GPU to stock settings after user confirmation."""
        if not messagebox.askokcancel(
            "Reset to Stock", "Reset GPU to stock settings?"
        ):
            return
        try:
            from ..detector import detect_gpus
            from ..backends.nvapi import NVAPIBackend
            from ..backends.nvidia_smi import NvidiaSMIBackend

            gpus = detect_gpus()
            if not gpus:
                messagebox.showerror("Error", "No GPUs detected.")
                return

            backends = [NVAPIBackend(), NvidiaSMIBackend()]
            for b in sorted(backends, key=lambda x: -x.priority):
                if b.is_available() and b.reset(gpus[0].index):
                    messagebox.showinfo("Success", f"GPU reset via {b.name}")
                    return
            messagebox.showwarning(
                "Warning", "No backend could reset the GPU."
            )
        except Exception as e:
            messagebox.showerror("Error", f"Reset failed: {e}")
