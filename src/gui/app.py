"""Main GUI application — manages screen navigation and lifecycle."""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Optional

from .theme import create_root
from .dashboard import DashboardScreen
from .optimization import OptimizationScreen
from .results import ResultsScreen
from .settings_screen import SettingsScreen
from ..tray import TrayIcon
from ..config import load_config, save_config
from ..monitor import GPUMonitor
from ..detector import detect_gpus


class GPUOptimizerApp:
    """Main application class."""

    def __init__(self) -> None:
        self.root = create_root()
        self._screens: dict[str, ttk.Frame] = {}
        self._current_screen: str = ""
        self._nav_buttons: dict[str, ttk.Button] = {}
        self._setup_layout()

        # Load config
        self._config = load_config()

        # Detect GPUs and create monitor (gracefully handle no GPU)
        gpus = detect_gpus()
        if gpus:
            self._monitor: Optional[GPUMonitor] = GPUMonitor(gpus[0].index)
        else:
            self._monitor = None

        # Create and register all screens
        dashboard = DashboardScreen(self.content_frame, self._config, self._monitor)
        self.register_screen("dashboard", dashboard)

        optimization = OptimizationScreen(self.content_frame, app_ref=self)
        self.register_screen("optimization", optimization)

        results = ResultsScreen(self.content_frame)
        results.load_from_config(self._config)
        self.register_screen("results", results)

        settings = SettingsScreen(self.content_frame, self._config, save_config)
        self.register_screen("settings", settings)

        # Create tray icon (started later in run())
        self._tray = TrayIcon(
            on_open_gui=self._show_window,
            on_reset=self._reset_to_stock,
            on_exit=self._quit,
        )

        # Override window close to hide to tray
        self.root.protocol("WM_DELETE_WINDOW", self._hide_to_tray)

    def _setup_layout(self) -> None:
        # Sidebar navigation (left)
        self._nav_frame = ttk.Frame(self.root, width=180)
        self._nav_frame.pack(side="left", fill="y", padx=(10, 0), pady=10)
        self._nav_frame.pack_propagate(False)

        # App title in sidebar
        title_label = ttk.Label(
            self._nav_frame,
            text="GPU Optimizer",
            font=("Segoe UI", 14, "bold"),
        )
        title_label.pack(pady=(10, 20))

        # Navigation buttons
        nav_items = [
            ("Dashboard", "dashboard"),
            ("Optimize", "optimization"),
            ("Results", "results"),
            ("Settings", "settings"),
        ]
        for label, screen_id in nav_items:
            btn = ttk.Button(
                self._nav_frame,
                text=label,
                command=lambda s=screen_id: self.show_screen(s),
                width=18,
            )
            btn.pack(fill="x", pady=2, padx=5)
            self._nav_buttons[screen_id] = btn

        # Separator between nav and content
        sep = ttk.Separator(self.root, orient="vertical")
        sep.pack(side="left", fill="y", padx=5)

        # Content area (right)
        self._content_frame = ttk.Frame(self.root)
        self._content_frame.pack(side="left", fill="both", expand=True, padx=10, pady=10)

    def register_screen(self, screen_id: str, frame: ttk.Frame) -> None:
        """Register a screen frame for navigation."""
        self._screens[screen_id] = frame

    def show_screen(self, screen_id: str) -> None:
        """Switch to the specified screen."""
        if self._current_screen and self._current_screen in self._screens:
            self._screens[self._current_screen].pack_forget()
        if screen_id in self._screens:
            self._screens[screen_id].pack(
                in_=self._content_frame, fill="both", expand=True
            )
            self._current_screen = screen_id

    @property
    def content_frame(self) -> ttk.Frame:
        """The content frame where screens are displayed."""
        return self._content_frame

    # ------------------------------------------------------------------
    # Tray / window lifecycle
    # ------------------------------------------------------------------

    def _hide_to_tray(self) -> None:
        """Hide window to tray instead of closing."""
        self.root.withdraw()

    def _show_window(self) -> None:
        """Show the main window."""
        self.root.deiconify()
        self.root.lift()
        self.root.focus_force()

    def _quit(self) -> None:
        """Full exit -- stop tray, destroy window."""
        self._tray.stop()
        self.root.destroy()

    def _reset_to_stock(self) -> None:
        """Reset GPU from tray menu."""
        # Import here to avoid circular imports
        from ..backends.nvapi import NVAPIBackend
        from ..backends.nvidia_smi import NvidiaSMIBackend

        backends = [NVAPIBackend(), NvidiaSMIBackend()]
        for b in sorted(backends, key=lambda x: -x.priority):
            if b.is_available() and b.reset(0):
                self._tray.show_notification(
                    "GPU Optimizer", f"GPU reset via {b.name}"
                )
                return
        self._tray.show_notification(
            "GPU Optimizer", "Reset failed -- no backend available"
        )

    def _update_tray_status(self) -> None:
        """Set tray icon color based on config state."""
        cfg = self._config
        if not cfg.per_gpu_results:
            self._tray.set_status("grey", "GPU Optimizer -- No settings applied")
        elif cfg.boot_apply.disabled:
            self._tray.set_status("red", "GPU Optimizer -- Auto-apply disabled")
        elif cfg.auto_apply_on_boot:
            self._tray.set_status("green", "GPU Optimizer -- Settings applied")
        else:
            self._tray.set_status(
                "yellow", "GPU Optimizer -- Settings applied (no auto-apply)"
            )

    # ------------------------------------------------------------------
    # Run
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Start the application."""
        self._tray.start()
        self._update_tray_status()
        self.show_screen("dashboard")

        # Start live monitoring on dashboard
        dashboard = self._screens.get("dashboard")
        if hasattr(dashboard, "start_live_monitor"):
            dashboard.start_live_monitor()

        self.root.mainloop()
