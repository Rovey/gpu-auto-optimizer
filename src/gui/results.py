"""Results screen — before/after comparison from last optimization."""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Optional


class ResultsScreen(ttk.Frame):
    """Shows optimization results with before/after comparison."""

    def __init__(self, parent: tk.Widget) -> None:
        super().__init__(parent)
        self._result = None
        self._build_ui()

    def _build_ui(self) -> None:
        self._header = ttk.Label(self, text="Optimization Results", font=("Segoe UI", 16, "bold"))
        self._header.pack(pady=(10, 5))

        self._subheader = ttk.Label(self, text="", font=("Segoe UI", 10))
        self._subheader.pack(pady=(0, 10))

        # Comparison table
        cols = ("Metric", "Baseline", "Achieved", "Delta")
        self._tree = ttk.Treeview(self, columns=cols, show="headings", height=5)
        for col in cols:
            self._tree.heading(col, text=col)
            self._tree.column(col, width=150, anchor="center")
        self._tree.column("Metric", anchor="w")
        self._tree.pack(fill="x", padx=20, pady=5)

        # Applied offsets section
        offsets_frame = ttk.LabelFrame(self, text="Applied Offsets")
        offsets_frame.pack(fill="x", padx=20, pady=10)
        self._offsets_label = ttk.Label(offsets_frame, text="", font=("Segoe UI", 10))
        self._offsets_label.pack(padx=10, pady=10)

        # Stability section
        stability_frame = ttk.LabelFrame(self, text="Stability")
        stability_frame.pack(fill="x", padx=20, pady=5)
        self._stability_label = ttk.Label(stability_frame, text="", font=("Segoe UI", 10))
        self._stability_label.pack(padx=10, pady=10)

        # Notes
        self._notes_label = ttk.Label(self, text="", font=("Segoe UI", 9), wraplength=600)
        self._notes_label.pack(padx=20, pady=5)

        # No results placeholder
        self._no_results = ttk.Label(
            self,
            text="No optimization results yet.\nRun an optimization from the Optimize tab.",
            font=("Segoe UI", 12),
            justify="center",
        )

    def show_result(self, result) -> None:
        """Populate all fields from a GPUOptimizationResult (or dict)."""
        self._result = result
        r = result if not isinstance(result, dict) else type('R', (), result)()

        self._no_results.pack_forget()
        self._header.pack(pady=(10, 5))

        gpu_name = getattr(r, 'gpu_name', 'Unknown GPU')
        risk = getattr(r, 'risk_level', '?')
        self._subheader.configure(text=f"{gpu_name} — {risk.upper()} profile")

        # Populate comparison table
        for item in self._tree.get_children():
            self._tree.delete(item)

        baseline_boost = getattr(r, 'baseline_boost_mhz', 0)
        achieved_boost = getattr(r, 'achieved_boost_mhz', 0)
        baseline_temp = getattr(r, 'baseline_temp_c', 0)
        achieved_temp = getattr(r, 'achieved_temp_c', 0)
        baseline_power = getattr(r, 'baseline_power_w', 0)
        achieved_power = getattr(r, 'achieved_power_w', 0)

        rows = [
            ("Core Clock (load)", f"{baseline_boost} MHz", f"{achieved_boost} MHz",
             f"{achieved_boost - baseline_boost:+d} MHz"),
            ("Temperature", f"{baseline_temp:.0f} °C", f"{achieved_temp:.0f} °C",
             f"{achieved_temp - baseline_temp:+.0f} °C"),
            ("Power Draw", f"{baseline_power:.0f} W", f"{achieved_power:.0f} W",
             f"{achieved_power - baseline_power:+.0f} W"),
        ]
        for row in rows:
            self._tree.insert("", "end", values=row)

        # Applied offsets
        core = getattr(r, 'core_offset_mhz', 0)
        mem = getattr(r, 'mem_offset_mhz', 0)
        volt = getattr(r, 'voltage_offset_mv', 0)
        power = getattr(r, 'power_limit_pct', 100)
        self._offsets_label.configure(
            text=f"Core: +{core} MHz    Memory: +{mem} MHz    Voltage: {volt:+d} mV    Power: {power}%"
        )

        # Stability
        passed = getattr(r, 'stability_passed', False)
        status = "STABLE — Settings verified" if passed else "UNSTABLE — Settings rolled back"
        self._stability_label.configure(text=status)

        # Notes
        notes = getattr(r, 'notes', '')
        self._notes_label.configure(text=notes if notes else "")

    def load_from_config(self, config) -> None:
        """Load last result from config, if available."""
        if config.per_gpu_results:
            last = list(config.per_gpu_results.values())[-1]
            self.show_result(last)
        else:
            self._show_no_results()

    def _show_no_results(self) -> None:
        """Show placeholder when no results exist."""
        for item in self._tree.get_children():
            self._tree.delete(item)
        self._subheader.configure(text="")
        self._offsets_label.configure(text="")
        self._stability_label.configure(text="")
        self._notes_label.configure(text="")
        self._no_results.pack(expand=True)
