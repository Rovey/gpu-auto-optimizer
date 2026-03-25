"""System tray icon with status display and context menu."""
from __future__ import annotations

import threading
from typing import Callable, Optional

try:
    from PIL import Image, ImageDraw
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False

try:
    import pystray
    _PYSTRAY_AVAILABLE = True
except ImportError:
    _PYSTRAY_AVAILABLE = False


def _create_icon_image(color: str = "grey", size: int = 64):
    """Generate a simple colored circle icon."""
    if not _PIL_AVAILABLE:
        return None
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    colors = {
        "green": (76, 175, 80),
        "yellow": (255, 193, 7),
        "red": (244, 67, 54),
        "grey": (158, 158, 158),
    }
    rgb = colors.get(color, colors["grey"])
    draw.ellipse([4, 4, size - 4, size - 4], fill=(*rgb, 255))
    return img


class TrayIcon:
    """System tray icon with status indication and context menu."""

    def __init__(
        self,
        on_open_gui: Callable[[], None],
        on_reset: Optional[Callable[[], None]] = None,
        on_toggle_auto_apply: Optional[Callable[[], None]] = None,
        on_exit: Optional[Callable[[], None]] = None,
    ) -> None:
        self._on_open = on_open_gui
        self._on_reset = on_reset
        self._on_toggle = on_toggle_auto_apply
        self._on_exit = on_exit
        self._icon = None
        self._status_text = "GPU Optimizer"
        self._color = "grey"

    def start(self) -> None:
        """Create and start the tray icon in a daemon thread."""
        if not _PYSTRAY_AVAILABLE or not _PIL_AVAILABLE:
            return

        menu = pystray.Menu(
            pystray.MenuItem(lambda _: self._status_text, None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Open GPU Optimizer", lambda: self._on_open()),
            pystray.MenuItem("Reset to Stock", lambda: self._on_reset() if self._on_reset else None),
            pystray.MenuItem(
                "Toggle Auto-Apply",
                lambda: self._on_toggle() if self._on_toggle else None,
            ),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Exit", lambda: self._on_exit() if self._on_exit else None),
        )

        self._icon = pystray.Icon(
            "gpu_optimizer",
            _create_icon_image(self._color),
            "GPU Optimizer",
            menu,
        )

        t = threading.Thread(target=self._icon.run, daemon=True)
        t.start()

    def stop(self) -> None:
        """Stop the tray icon."""
        if self._icon:
            try:
                self._icon.stop()
            except Exception:
                pass

    def set_status(self, color: str, tooltip: str = "") -> None:
        """Update icon color and tooltip text."""
        self._color = color
        if tooltip:
            self._status_text = tooltip
        if self._icon:
            img = _create_icon_image(color)
            if img:
                self._icon.icon = img
            if tooltip:
                self._icon.title = tooltip

    def show_notification(self, title: str, message: str) -> None:
        """Show a Windows toast notification via pystray."""
        if self._icon:
            try:
                self._icon.notify(message, title)
            except Exception:
                pass
