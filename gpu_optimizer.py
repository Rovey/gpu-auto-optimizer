#!/usr/bin/env python3
"""GPU Optimizer -- launch the GUI application."""
import sys
from pathlib import Path

# Ensure src/ is importable
sys.path.insert(0, str(Path(__file__).parent))

from src.gui.app import GPUOptimizerApp


def main():
    app = GPUOptimizerApp()
    app.run()


if __name__ == "__main__":
    main()
