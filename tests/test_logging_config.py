# tests/test_logging_config.py
import os
from pathlib import Path

def test_setup_logging_creates_log_dir(tmp_path):
    from src.logging_config import setup_logging
    log_dir = tmp_path / "logs"
    logger = setup_logging(log_dir=str(log_dir), name="test")
    assert log_dir.exists()

def test_optimization_log_has_timestamp_name(tmp_path):
    from src.logging_config import create_optimization_log
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    path = create_optimization_log(str(log_dir))
    assert "optimize_" in os.path.basename(path)
    assert path.endswith(".log")

def test_prune_logs_removes_oldest(tmp_path):
    from src.logging_config import prune_logs
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    for i in range(15):
        f = log_dir / f"optimize_{i:04d}.log"
        f.write_bytes(b"x" * (4 * 1024 * 1024))  # 4 MB each = 60 MB total
    prune_logs(str(log_dir), max_total_bytes=50 * 1024 * 1024)
    total = sum(f.stat().st_size for f in log_dir.iterdir())
    assert total <= 50 * 1024 * 1024

def test_prune_keeps_max_n_optimization_logs(tmp_path):
    from src.logging_config import prune_logs
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    for i in range(15):
        f = log_dir / f"optimize_{i:04d}.log"
        f.write_text("small")
    prune_logs(str(log_dir), max_optimization_logs=10)
    opt_logs = [f for f in log_dir.iterdir() if f.name.startswith("optimize_")]
    assert len(opt_logs) <= 10
