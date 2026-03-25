"""
Shared GUI widgets for use across all screens.

All widgets use ttk for Sun Valley theme compatibility.
Duck typing is used throughout — no imports from monitor or config modules.
"""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk


# ---------------------------------------------------------------------------
# StatusIndicator — coloured circle on a canvas
# ---------------------------------------------------------------------------

_STATUS_COLORS = {
    "green":  "#2ecc71",
    "yellow": "#f1c40f",
    "red":    "#e74c3c",
    "grey":   "#95a5a6",
}


class StatusIndicator(tk.Canvas):
    """A small canvas that draws a single coloured circle.

    Parameters
    ----------
    parent : widget
        Parent Tkinter container.
    size : int
        Diameter of the circle in pixels (default 16).
    """

    def __init__(self, parent: tk.Widget, size: int = 16, **kw):
        kw.setdefault("width", size)
        kw.setdefault("height", size)
        kw.setdefault("highlightthickness", 0)
        kw.setdefault("borderwidth", 0)
        super().__init__(parent, **kw)
        self._size = size
        pad = 2
        self._dot = self.create_oval(pad, pad, size - pad, size - pad,
                                     fill=_STATUS_COLORS["grey"], outline="")

    def set_status(self, color: str) -> None:
        """Update the circle fill.  *color* is 'green', 'yellow', 'red', or 'grey'."""
        fill = _STATUS_COLORS.get(color, color)
        self.itemconfig(self._dot, fill=fill)


# ---------------------------------------------------------------------------
# GPUMetricsCard — live GPU stats in a labelled grid
# ---------------------------------------------------------------------------

class GPUMetricsCard(ttk.LabelFrame):
    """LabelFrame showing live GPU statistics in a two-column grid.

    Call :meth:`update` with a ``GPUMetrics``-like object (or dict) to refresh
    all displayed values.
    """

    _FIELDS = [
        ("Core Clock", "core_clock_mhz", "{} MHz"),
        ("Mem Clock",  "mem_clock_mhz",  "{} MHz"),
        ("Temperature", "temp_c",         "{:.0f} \u00b0C"),
        ("Power",       "power_w",        "{:.1f} W"),
        ("Fan",         "fan_speed_pct",  "{} %"),
        ("Utilization", "gpu_util_pct",   "{} %"),
    ]

    def __init__(self, parent: tk.Widget, **kw):
        kw.setdefault("text", "GPU Metrics")
        kw.setdefault("padding", 10)
        super().__init__(parent, **kw)

        self._value_labels: dict[str, ttk.Label] = {}

        for row, (label_text, attr, _fmt) in enumerate(self._FIELDS):
            ttk.Label(self, text=label_text + ":").grid(
                row=row, column=0, sticky="w", padx=(0, 10), pady=2,
            )
            val = ttk.Label(self, text="--")
            val.grid(row=row, column=1, sticky="e", pady=2)
            self._value_labels[attr] = val

        self.columnconfigure(1, weight=1)

    def update(self, metrics) -> None:  # noqa: A003 (shadows builtin)
        """Refresh displayed values from *metrics*.

        *metrics* may be any object whose attributes match the GPUMetrics
        field names, **or** a plain dict with the same keys.
        """
        for _label_text, attr, fmt in self._FIELDS:
            if isinstance(metrics, dict):
                raw = metrics.get(attr)
            else:
                raw = getattr(metrics, attr, None)

            if raw is None:
                text = "--"
            else:
                try:
                    text = fmt.format(raw)
                except (ValueError, TypeError):
                    text = str(raw)
            self._value_labels[attr].configure(text=text)


# ---------------------------------------------------------------------------
# LogViewer — scrollable, read-only text pane
# ---------------------------------------------------------------------------

class LogViewer(ttk.Frame):
    """Scrollable read-only text widget for log output.

    Parameters
    ----------
    parent : widget
        Parent Tkinter container.
    max_lines : int
        Maximum number of lines to keep (oldest lines are trimmed).
    """

    def __init__(self, parent: tk.Widget, max_lines: int = 100, **kw):
        super().__init__(parent, **kw)
        self._max_lines = max_lines

        self._text = tk.Text(self, wrap="word", state="disabled",
                             height=10, borderwidth=0, highlightthickness=0)
        self._scrollbar = ttk.Scrollbar(self, orient="vertical",
                                        command=self._text.yview)
        self._text.configure(yscrollcommand=self._scrollbar.set)

        self._text.pack(side="left", fill="both", expand=True)
        self._scrollbar.pack(side="right", fill="y")

    def append(self, text: str) -> None:
        """Add *text* as a new line, auto-scroll, and trim if necessary."""
        self._text.configure(state="normal")
        self._text.insert("end", text.rstrip("\n") + "\n")
        # Trim oldest lines if over the limit
        line_count = int(self._text.index("end-1c").split(".")[0])
        if line_count > self._max_lines:
            excess = line_count - self._max_lines
            self._text.delete("1.0", f"{excess + 1}.0")
        self._text.configure(state="disabled")
        self._text.see("end")

    def clear(self) -> None:
        """Remove all text."""
        self._text.configure(state="normal")
        self._text.delete("1.0", "end")
        self._text.configure(state="disabled")


# ---------------------------------------------------------------------------
# OffsetDisplay — currently applied GPU offsets
# ---------------------------------------------------------------------------

class OffsetDisplay(ttk.Frame):
    """Frame showing the currently applied GPU offsets (core, mem, voltage, power).

    Call :meth:`update` with a ``GPUOptimizationResult``-like object (or dict)
    to refresh displayed values.
    """

    _FIELDS = [
        ("Core",    "core_offset_mhz",   "{:+d} MHz"),
        ("Memory",  "mem_offset_mhz",    "{:+d} MHz"),
        ("Voltage", "voltage_offset_mv",  "{:+d} mV"),
        ("Power",   "power_limit_pct",    "{} %"),
    ]

    def __init__(self, parent: tk.Widget, **kw):
        super().__init__(parent, **kw)

        self._value_labels: dict[str, ttk.Label] = {}

        for row, (label_text, attr, _fmt) in enumerate(self._FIELDS):
            ttk.Label(self, text=label_text + ":").grid(
                row=row, column=0, sticky="w", padx=(0, 10), pady=2,
            )
            val = ttk.Label(self, text="--")
            val.grid(row=row, column=1, sticky="e", pady=2)
            self._value_labels[attr] = val

        self.columnconfigure(1, weight=1)

    def update(self, result) -> None:  # noqa: A003
        """Refresh labels from *result* (``GPUOptimizationResult``-like or dict)."""
        for _label_text, attr, fmt in self._FIELDS:
            if isinstance(result, dict):
                raw = result.get(attr)
            else:
                raw = getattr(result, attr, None)

            if raw is None:
                text = "--"
            else:
                try:
                    text = fmt.format(raw)
                except (ValueError, TypeError):
                    text = str(raw)
            self._value_labels[attr].configure(text=text)


# ---------------------------------------------------------------------------
# StepProgressList — Treeview of optimization step results
# ---------------------------------------------------------------------------

class StepProgressList(ttk.Frame):
    """Treeview widget showing optimisation step results (Step / PASS|FAIL).

    Methods
    -------
    add_step(description, passed)
        Insert a new row.
    clear()
        Remove all rows.
    """

    def __init__(self, parent: tk.Widget, **kw):
        super().__init__(parent, **kw)

        columns = ("step", "result")
        self._tree = ttk.Treeview(self, columns=columns, show="headings",
                                  selectmode="none", height=8)
        self._tree.heading("step", text="Step")
        self._tree.heading("result", text="Result")
        self._tree.column("step", stretch=True, minwidth=200)
        self._tree.column("result", width=80, anchor="center", stretch=False)

        scrollbar = ttk.Scrollbar(self, orient="vertical",
                                  command=self._tree.yview)
        self._tree.configure(yscrollcommand=scrollbar.set)

        self._tree.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        # Tag-based colouring for pass / fail rows
        self._tree.tag_configure("pass", foreground="#2ecc71")
        self._tree.tag_configure("fail", foreground="#e74c3c")

    def add_step(self, description: str, passed: bool) -> None:
        """Insert a row showing *description* and PASS/FAIL."""
        result_text = "PASS" if passed else "FAIL"
        tag = "pass" if passed else "fail"
        self._tree.insert("", "end", values=(description, result_text),
                          tags=(tag,))
        # Auto-scroll to the latest entry
        children = self._tree.get_children()
        if children:
            self._tree.see(children[-1])

    def clear(self) -> None:
        """Remove all rows."""
        for item in self._tree.get_children():
            self._tree.delete(item)
