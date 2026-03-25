# tests/test_optimizer.py
import pytest
import threading
from unittest.mock import MagicMock, patch
from src.config import RiskLevel, RISK_PROFILES
from src.backends.base import AppliedSettings


@pytest.fixture
def mock_optimizer():
    """Create a GPUOptimizer with mocked backend."""
    mock_gpu = MagicMock()
    mock_gpu.index = 0
    mock_gpu.name = "Test GPU"
    mock_gpu.supports_uv = True

    mock_backend = MagicMock()
    mock_backend.is_available.return_value = True
    mock_backend.supports_core_oc.return_value = True
    mock_backend.supports_mem_oc.return_value = True
    mock_backend.supports_voltage.return_value = True

    with patch("src.optimizer._best_backend", return_value=mock_backend):
        from src.optimizer import GPUOptimizer
        opt = GPUOptimizer(mock_gpu, RiskLevel.BALANCED)
    return opt


def test_apply_checks_return_value(mock_optimizer):
    """Optimizer should raise if backend.apply() returns success=False."""
    mock_optimizer._backend.apply.return_value = AppliedSettings(
        success=False, notes="NVAPI failed"
    )
    with pytest.raises(RuntimeError, match="NVAPI failed"):
        mock_optimizer._apply()


def test_apply_checks_verification_when_oc_offsets_set(mock_optimizer):
    """Optimizer should raise if read-back verification fails with OC offsets."""
    mock_optimizer._core_offset_mhz = 100
    mock_optimizer._backend.apply.return_value = AppliedSettings(
        success=True, verified=False, notes="Read-back mismatch"
    )
    with pytest.raises(RuntimeError, match="(?i)verification"):
        mock_optimizer._apply()


def test_apply_skips_verification_for_power_only(mock_optimizer):
    """Power-limit-only changes should not require read-back verification."""
    mock_optimizer._core_offset_mhz = 0
    mock_optimizer._mem_offset_mhz = 0
    mock_optimizer._voltage_offset_mv = 0
    mock_optimizer._backend.apply.return_value = AppliedSettings(
        success=True, verified=False, notes="Power only"
    )
    result = mock_optimizer._apply()
    assert result.success is True


def test_measure_under_load_method_exists(mock_optimizer):
    assert hasattr(mock_optimizer, "_measure_under_load")


def test_cancel_mechanism(mock_optimizer):
    assert hasattr(mock_optimizer, '_cancel_event')
    assert not mock_optimizer._cancel_event.is_set()
    mock_optimizer.cancel()
    assert mock_optimizer._cancel_event.is_set()
