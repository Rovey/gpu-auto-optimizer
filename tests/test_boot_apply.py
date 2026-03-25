# tests/test_boot_apply.py
from unittest.mock import patch, MagicMock
from src.config import UserConfig, BootApplyState

def test_skip_if_auto_apply_disabled():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = False
    result = should_apply(cfg)
    assert result.skip is True
    assert "disabled" in result.reason.lower()

def test_skip_if_3_consecutive_failures():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.consecutive_failures = 3
    cfg.boot_apply.disabled = True
    result = should_apply(cfg)
    assert result.skip is True
    assert "strike" in result.reason.lower() or "disabled" in result.reason.lower()

def test_skip_if_gpu_uuid_changed():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.gpu_uuid = "GPU-old-uuid"
    cfg.per_gpu_results = {"MyGPU": {"core_offset_mhz": 100}}
    result = should_apply(cfg, current_gpu_uuid="GPU-new-uuid")
    assert result.skip is True
    assert "hardware" in result.reason.lower() or "uuid" in result.reason.lower()

def test_allow_if_driver_changed():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.gpu_uuid = "GPU-abc"
    cfg.boot_apply.driver_version = "555.00"
    cfg.per_gpu_results = {"MyGPU": {"core_offset_mhz": 100}}
    result = should_apply(cfg, current_gpu_uuid="GPU-abc", current_driver="560.00")
    assert result.skip is False
    assert "driver" in result.warning.lower()

def test_increment_failure_on_stress_fail():
    from src.boot_apply import record_boot_result
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 1
    record_boot_result(cfg, success=False, details="Stress test failed")
    assert cfg.boot_apply.consecutive_failures == 2
    assert cfg.boot_apply.disabled is False

def test_disable_on_third_failure():
    from src.boot_apply import record_boot_result
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 2
    record_boot_result(cfg, success=False, details="Stress test failed")
    assert cfg.boot_apply.consecutive_failures == 3
    assert cfg.boot_apply.disabled is True

def test_reset_failures_on_success():
    from src.boot_apply import record_boot_result
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 2
    record_boot_result(cfg, success=True, details="OK")
    assert cfg.boot_apply.consecutive_failures == 0
    assert cfg.boot_apply.disabled is False

def test_no_saved_results_skips():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.gpu_uuid = "GPU-abc"
    cfg.per_gpu_results = {}
    result = should_apply(cfg, current_gpu_uuid="GPU-abc")
    assert result.skip is True
    assert "no saved" in result.reason.lower()
