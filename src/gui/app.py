"""Main GUI application — manages screen navigation and lifecycle."""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Optional

from .theme import create_root


class GPUOptimizerApp:
    """Main application class."""

    def __init__(self) -> None:
        self.root = create_root()
        self._screens: dict[str, ttk.Frame] = {}
        self._current_screen: str = ""
        self._nav_buttons: dict[str, ttk.Button] = {}
        self._setup_layout()

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

    def run(self) -> None:
        """Start the application."""
        if "dashboard" in self._screens:
            self.show_screen("dashboard")
        self.root.mainloop()
