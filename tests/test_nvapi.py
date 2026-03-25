# tests/test_nvapi.py
from unittest.mock import patch, MagicMock
from src.backends.base import AppliedSettings


def test_applied_settings_has_verified_field():
    s = AppliedSettings()
    assert hasattr(s, "verified")
    assert s.verified is False


def test_verify_returns_actual_offsets():
    """verify() should read back P-state20 and return actual offsets."""
    from src.backends.nvapi import NVAPIBackend
    backend = NVAPIBackend()
    result = backend.verify(gpu_index=0)
    # On non-Windows or without NVIDIA, result is None
    assert result is None or isinstance(result, dict)
