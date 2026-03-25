# tests/test_config.py
import json
import os
import tempfile
from src.config import (
    UserConfig, BootApplyState, load_config, save_config,
    get_app_dir, get_config_dir, get_log_dir,
)

def test_user_config_has_boot_apply_state():
    cfg = UserConfig()
    assert cfg.auto_apply_on_boot is False
    assert isinstance(cfg.boot_apply, BootApplyState)
    assert cfg.boot_apply.consecutive_failures == 0
    assert cfg.boot_apply.disabled is False
    assert cfg.boot_apply.gpu_uuid == ""
    assert cfg.boot_apply.driver_version == ""

def test_boot_apply_state_serialization(tmp_path):
    path = tmp_path / "config.json"
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 2
    cfg.boot_apply.gpu_uuid = "GPU-abc-123"
    cfg.boot_apply.driver_version = "560.70"
    save_config(cfg, str(path))

    loaded = load_config(str(path))
    assert loaded.boot_apply.consecutive_failures == 2
    assert loaded.boot_apply.gpu_uuid == "GPU-abc-123"
    assert loaded.boot_apply.driver_version == "560.70"

def test_app_dir_uses_localappdata(monkeypatch):
    monkeypatch.setenv("LOCALAPPDATA", "C:\\Users\\Test\\AppData\\Local")
    assert get_app_dir() == os.path.join("C:\\Users\\Test\\AppData\\Local", "GPUOptimizer")

def test_config_dir():
    app_dir = get_app_dir()
    assert get_config_dir() == os.path.join(app_dir, "config")

def test_log_dir():
    app_dir = get_app_dir()
    assert get_log_dir() == os.path.join(app_dir, "logs")

def test_load_missing_config_returns_defaults(tmp_path):
    path = tmp_path / "nonexistent.json"
    cfg = load_config(str(path))
    assert cfg.risk_level == "balanced"
    assert cfg.auto_apply_on_boot is False

def test_save_and_load_roundtrip(tmp_path):
    path = tmp_path / "config.json"
    cfg = UserConfig()
    cfg.risk_level = "performance"
    cfg.auto_apply_on_boot = True
    cfg.per_gpu_results = {"GPU0": {"core_offset_mhz": 100}}
    save_config(cfg, str(path))

    loaded = load_config(str(path))
    assert loaded.risk_level == "performance"
    assert loaded.auto_apply_on_boot is True
    assert loaded.per_gpu_results["GPU0"]["core_offset_mhz"] == 100
