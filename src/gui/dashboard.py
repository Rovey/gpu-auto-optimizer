"""Dashboard screen -- live GPU stats, applied settings, auto-apply status, boot log."""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Optional

from .widgets import GPUMetricsCard, OffsetDisplay, StatusIndicator, LogViewer


class DashboardScreen(ttk.Frame):
    """Dashboard -- home screen showing current GPU state and optimization status."""

    def __init__(self, parent: tk.Widget, config, monitor=None) -> None:
        super().__init__(parent)
        self._config = config
        self._monitor = monitor
        self._polling_id: Optional[str] = None
        self._build_ui()
        self.refresh()

    def _build_ui(self) -> None:
        # 2x2 grid
        self.columnconfigure(0, weight=1)
        self.columnconfigure(1, weight=1)
        self.rowconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        # Top-left: GPU Status
        self._metrics_card = GPUMetricsCard(self)
        self._metrics_card.grid(row=0, column=0, sticky="nsew", padx=(0, 5), pady=(0, 5))

        # Top-right: Applied Settings
        settings_frame = ttk.LabelFrame(self, text="Applied Settings")
        settings_frame.grid(row=0, column=1, sticky="nsew", padx=(5, 0), pady=(0, 5))
        self._offset_display = OffsetDisplay(settings_frame)
        self._offset_display.pack(fill="both", expand=True, padx=10, pady=10)
        self._no_results_label = ttk.Label(
            settings_frame, text="No optimization run yet.", font=("Segoe UI", 10)
        )

        # Bottom-left: Auto-Apply Status
        status_frame = ttk.LabelFrame(self, text="Auto-Apply Status")
        status_frame.grid(row=1, column=0, sticky="nsew", padx=(0, 5), pady=(5, 0))

        status_inner = ttk.Frame(status_frame)
        status_inner.pack(fill="both", expand=True, padx=10, pady=10)

        self._status_indicator = StatusIndicator(status_inner)
        self._status_indicator.pack(side="left", padx=(0, 10))

        self._status_labels_frame = ttk.Frame(status_inner)
        self._status_labels_frame.pack(side="left", fill="both", expand=True)

        self._auto_apply_label = ttk.Label(self._status_labels_frame, text="Auto-Apply: Disabled")
        self._auto_apply_label.pack(anchor="w")
        self._strike_label = ttk.Label(self._status_labels_frame, text="Strikes: 0/3")
        self._strike_label.pack(anchor="w")
        self._last_boot_label = ttk.Label(self._status_labels_frame, text="Last boot: N/A")
        self._last_boot_label.pack(anchor="w")

        # Bottom-right: Boot Log
        log_frame = ttk.LabelFrame(self, text="Boot Log")
        log_frame.grid(row=1, column=1, sticky="nsew", padx=(5, 0), pady=(5, 0))
        self._boot_log = LogViewer(log_frame, max_lines=50)
        self._boot_log.pack(fill="both", expand=True, padx=5, pady=5)

    def refresh(self) -> None:
        """Reload config and update all sections."""
        cfg = self._config

        # Applied settings
        if cfg.per_gpu_results:
            last_result = list(cfg.per_gpu_results.values())[-1]
            self._offset_display.update(last_result)
            self._no_results_label.pack_forget()
            self._offset_display.pack(fill="both", expand=True, padx=10, pady=10)
        else:
            self._offset_display.pack_forget()
            self._no_results_label.pack(fill="both", expand=True, padx=10, pady=10)

        # Auto-apply status
        if cfg.auto_apply_on_boot:
            if cfg.boot_apply.disabled:
                self._status_indicator.set_status("red")
                self._auto_apply_label.configure(text="Auto-Apply: DISABLED (3 strikes)")
            elif cfg.boot_apply.consecutive_failures > 0:
                self._status_indicator.set_status("yellow")
                self._auto_apply_label.configure(text="Auto-Apply: Enabled (warnings)")
            else:
                self._status_indicator.set_status("green")
                self._auto_apply_label.configure(text="Auto-Apply: Enabled")
        else:
            self._status_indicator.set_status("grey")
            self._auto_apply_label.configure(text="Auto-Apply: Disabled")

        self._strike_label.configure(
            text=f"Strikes: {cfg.boot_apply.consecutive_failures}/3"
        )

        last_time = cfg.boot_apply.last_apply_time or "N/A"
        last_result_text = cfg.boot_apply.last_apply_result or "N/A"
        self._last_boot_label.configure(text=f"Last boot: {last_result_text} ({last_time})")

        # Boot log
        self._boot_log.clear()
        for entry in cfg.boot_apply.boot_log[-10:]:
            ts = entry.get("timestamp", "?")
            result = entry.get("result", "?")
            details = entry.get("details", "")
            self._boot_log.append(f"[{ts}] {result}: {details}")

    def start_live_monitor(self) -> None:
        """Start polling GPU metrics every second."""
        if self._monitor is None:
            return
        self._poll_metrics()

    def stop_live_monitor(self) -> None:
        """Stop the polling loop."""
        if self._polling_id is not None:
            self.after_cancel(self._polling_id)
            self._polling_id = None

    def _poll_metrics(self) -> None:
        if self._monitor is None:
            return
        try:
            m = self._monitor.read_once()
            self._metrics_card.update(m)
        except Exception:
            pass
        self._polling_id = self.after(1000, self._poll_metrics)
