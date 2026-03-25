# tests/test_stability.py
import numpy as np
from unittest.mock import patch, MagicMock


def test_correctness_check_passes_with_correct_result():
    """Verify _verify_correctness returns True when GPU result matches CPU."""
    from src.stability import _verify_correctness
    a = np.random.random((64, 64)).astype(np.float32)
    b = np.random.random((64, 64)).astype(np.float32)
    cpu_result = a @ b
    assert _verify_correctness(cpu_result, cpu_result.copy()) is True


def test_correctness_check_detects_corruption():
    """If GPU returns wrong values, _verify_correctness should return False."""
    from src.stability import _verify_correctness
    a = np.random.random((64, 64)).astype(np.float32)
    b = np.random.random((64, 64)).astype(np.float32)
    cpu_result = a @ b
    corrupted = cpu_result.copy()
    corrupted[0, 0] += 1000.0
    assert _verify_correctness(cpu_result, corrupted) is False


def test_stability_result_fields():
    from src.stability import StabilityResult
    r = StabilityResult()
    assert r.passed is False
    assert r.valid_load is True
    assert r.correctness_passed is True
    assert r.stress_backend == ""


def test_cupy_required_fails_when_unavailable():
    """If CuPy is not available, StabilityTester.run() should fail with clear message."""
    from src.stability import StabilityTester
    with patch.object(StabilityTester, '_cupy_available', return_value=False):
        tester = StabilityTester(gpu_index=0)
        result = tester.run(duration_sec=10)
        assert not result.passed
        assert "CuPy" in result.failure_reason
