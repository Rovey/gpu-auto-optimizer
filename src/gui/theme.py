"""Sun Valley theme initialization for Tkinter."""
import tkinter as tk
from tkinter import ttk


def apply_theme(root: tk.Tk, mode: str = "dark") -> None:
    """Apply Sun Valley ttk theme. mode = 'dark' or 'light'."""
    import sv_ttk
    sv_ttk.set_theme(mode)


def create_root(title: str = "GPU Optimizer") -> tk.Tk:
    """Create and configure the root Tk window."""
    root = tk.Tk()
    root.title(title)
    root.geometry("900x650")
    root.minsize(800, 550)
    try:
        apply_theme(root, mode="dark")
    except ImportError:
        pass  # sv_ttk not installed — use default theme
    return root
