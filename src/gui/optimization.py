"""Optimization screen — risk selection, live progress, results."""
from __future__ import annotations

import threading
import tkinter as tk
from tkinter import ttk, messagebox
from typing import Optional, Callable

from .widgets import GPUMetricsCard, StepProgressList


class OptimizationScreen(ttk.Frame):
    """Optimization flow: risk selection -> live progress -> completion."""

    def __init__(self, parent: tk.Widget, app_ref=None) -> None:
        super().__init__(parent)
        self._app = app_ref
        self._optimizer = None
        self._optimizer_thread: Optional[threading.Thread] = None
        self._state = "select"  # select | running | complete
        self._build_select_ui()

    def _build_select_ui(self) -> None:
        """Build the risk profile selection screen."""
        self._clear()
        self._state = "select"

        ttk.Label(self, text="Select Risk Profile", font=("Segoe UI", 16, "bold")).pack(pady=(10, 20))

        from src.config import RiskLevel, RISK_PROFILES

        cards_frame = ttk.Frame(self)
        cards_frame.pack(fill="both", expand=True)

        for i, (level, profile) in enumerate(RISK_PROFILES.items()):
            card = ttk.LabelFrame(cards_frame, text=profile["label"])
            card.pack(fill="x", padx=10, pady=5)

            desc = ttk.Label(card, text=profile["description"], wraplength=500)
            desc.pack(anchor="w", padx=10, pady=(5, 0))

            limits = (
                f"Core: +{profile['core_offset_mhz_max']} MHz  |  "
                f"Mem: +{profile['mem_offset_mhz_max']} MHz  |  "
                f"Volt: {profile['voltage_offset_mv_min']} mV"
            )
            ttk.Label(card, text=limits, font=("Segoe UI", 9)).pack(anchor="w", padx=10, pady=(2, 5))

            btn = ttk.Button(
                card,
                text=f"Start {profile['label']}",
                command=lambda lv=level, pf=profile: self._on_start(lv, pf),
            )
            btn.pack(anchor="e", padx=10, pady=5)

    def _on_start(self, level, profile) -> None:
        """Handle start button click — show warning, then begin."""
        warning = profile.get("warning")
        if warning:
            if not messagebox.askokcancel("Warning", warning):
                return
        self._start_optimization(level)

    def _start_optimization(self, risk_level) -> None:
        """Switch to running state and launch optimizer in background thread."""
        self._build_running_ui()

        def _run():
            try:
                from src.detector import detect_gpus
                from src.optimizer import GPUOptimizer
                from src.config import save_config, load_config, save_result

                gpus = detect_gpus()
                if not gpus:
                    self.after(0, lambda: self._on_error("No NVIDIA GPUs detected."))
                    return

                gpu = gpus[0]  # optimize first GPU
                self._optimizer = GPUOptimizer(gpu, risk_level, self._on_progress)
                result = self._optimizer.run()
                cfg = load_config()
                save_result(cfg, result)
                self.after(0, lambda: self._on_complete(result))
            except Exception as exc:
                self.after(0, lambda e=str(exc): self._on_error(e))

        self._optimizer_thread = threading.Thread(target=_run, daemon=True)
        self._optimizer_thread.start()

    def _build_running_ui(self) -> None:
        """Build the live progress UI."""
        self._clear()
        self._state = "running"

        self._phase_label = ttk.Label(self, text="Initializing...", font=("Segoe UI", 14, "bold"))
        self._phase_label.pack(pady=(10, 5))

        self._progress = ttk.Progressbar(self, mode="determinate", length=400)
        self._progress.pack(pady=5)

        mid_frame = ttk.Frame(self)
        mid_frame.pack(fill="both", expand=True, pady=5)
        mid_frame.columnconfigure(0, weight=1)
        mid_frame.columnconfigure(1, weight=1)

        self._step_list = StepProgressList(mid_frame)
        self._step_list.grid(row=0, column=0, sticky="nsew", padx=(0, 5))

        self._metrics_card = GPUMetricsCard(mid_frame)
        self._metrics_card.grid(row=0, column=1, sticky="nsew", padx=(5, 0))

        self._cancel_btn = ttk.Button(self, text="Cancel", command=self._on_cancel)
        self._cancel_btn.pack(pady=10)

    def _on_progress(self, phase: str, step: int, total: int, metrics=None) -> None:
        """Progress callback from optimizer (called from optimizer thread)."""
        def _update():
            self._phase_label.configure(text=phase)
            if total > 0:
                self._progress["value"] = (step / total) * 100
            if metrics:
                self._metrics_card.update(metrics)
            # Add step to list if it contains pass/fail info
            if "PASS" in phase.upper():
                self._step_list.add_step(phase, True)
            elif "FAIL" in phase.upper():
                self._step_list.add_step(phase, False)
        self.after(0, _update)

    def _on_cancel(self) -> None:
        """Cancel the running optimization."""
        if self._optimizer:
            self._optimizer.cancel()
        self._cancel_btn.configure(state="disabled", text="Cancelling...")

    def _on_complete(self, result) -> None:
        """Switch to completion state."""
        self._clear()
        self._state = "complete"

        ttk.Label(self, text="Optimization Complete!", font=("Segoe UI", 16, "bold")).pack(pady=(20, 10))

        info = ttk.Frame(self)
        info.pack(pady=10)

        labels = [
            f"Core Offset: +{result.core_offset_mhz} MHz",
            f"Memory Offset: +{result.mem_offset_mhz} MHz",
            f"Voltage Offset: {result.voltage_offset_mv:+d} mV",
            f"Power Limit: {result.power_limit_pct}%",
        ]
        for text in labels:
            ttk.Label(info, text=text, font=("Segoe UI", 11)).pack(anchor="w")

        status = "Stable" if result.stability_passed else "Unstable (rolled back)"
        color = "green" if result.stability_passed else "red"
        ttk.Label(
            self, text=f"Status: {status}", font=("Segoe UI", 12, "bold"),
        ).pack(pady=10)

        btns = ttk.Frame(self)
        btns.pack(pady=10)
        ttk.Button(
            btns, text="View Results",
            command=lambda: self._app.show_screen("results") if self._app else None,
        ).pack(side="left", padx=5)
        ttk.Button(
            btns, text="Back to Dashboard",
            command=lambda: self._app.show_screen("dashboard") if self._app else None,
        ).pack(side="left", padx=5)

    def _on_error(self, message: str) -> None:
        """Show error and return to selection."""
        messagebox.showerror("Optimization Error", message)
        self._build_select_ui()

    def _clear(self) -> None:
        """Remove all child widgets."""
        for w in self.winfo_children():
            w.destroy()
