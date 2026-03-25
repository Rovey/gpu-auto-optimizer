"""Centralized logging configuration with rotation and size cap."""
import logging
import logging.handlers
import os
from datetime import datetime
from pathlib import Path


def setup_logging(log_dir: str, name: str = "gpu_optimizer") -> logging.Logger:
    """Create log directory and return a configured logger."""
    os.makedirs(log_dir, exist_ok=True)
    logger = logging.getLogger(name)
    if not logger.handlers:
        logger.setLevel(logging.DEBUG)
        # Console handler
        ch = logging.StreamHandler()
        ch.setLevel(logging.INFO)
        ch.setFormatter(logging.Formatter("%(levelname)s: %(message)s"))
        logger.addHandler(ch)
    return logger


def create_optimization_log(log_dir: str) -> str:
    """Create a new per-run optimization log file and return its path."""
    os.makedirs(log_dir, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = os.path.join(log_dir, f"optimize_{timestamp}.log")
    return path


def setup_boot_apply_log(log_dir: str) -> logging.Logger:
    """Set up rotating logger for boot-apply events."""
    os.makedirs(log_dir, exist_ok=True)
    logger = logging.getLogger("boot_apply")
    if not logger.handlers:
        logger.setLevel(logging.DEBUG)
        handler = logging.handlers.RotatingFileHandler(
            os.path.join(log_dir, "boot_apply.log"),
            maxBytes=1 * 1024 * 1024,  # 1 MB
            backupCount=3,
        )
        handler.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)s %(message)s"
        ))
        logger.addHandler(handler)
    return logger


def prune_logs(
    log_dir: str,
    max_total_bytes: int = 50 * 1024 * 1024,
    max_optimization_logs: int = 10,
) -> None:
    """Delete oldest logs to stay under count and size limits."""
    log_path = Path(log_dir)
    if not log_path.exists():
        return

    # Phase 1: prune optimization logs by count
    opt_logs = sorted(
        [f for f in log_path.iterdir() if f.name.startswith("optimize_") and f.is_file()],
        key=lambda f: f.stat().st_mtime,
    )
    while len(opt_logs) > max_optimization_logs:
        opt_logs[0].unlink()
        opt_logs.pop(0)

    # Phase 2: prune all logs by total size
    all_files = sorted(
        [f for f in log_path.iterdir() if f.is_file()],
        key=lambda f: f.stat().st_mtime,
    )
    total = sum(f.stat().st_size for f in all_files)
    while total > max_total_bytes and all_files:
        removed = all_files.pop(0)
        total -= removed.stat().st_size
        removed.unlink()
