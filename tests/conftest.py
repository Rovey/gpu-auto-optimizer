"""Shared pytest fixtures for GPU optimizer tests."""
import pytest
from unittest.mock import MagicMock, patch

@pytest.fixture
def mock_gpu():
    gpu = MagicMock()
    gpu.index = 0
    gpu.name = "Test GPU"
    gpu.vendor = "NVIDIA"
    gpu.architecture = "Turing"
    gpu.uuid = "GPU-test-uuid-123"
    gpu.driver_version = "560.70"
    gpu.supports_oc = True
    gpu.supports_uv = True
    gpu.supports_mem_oc = True
    return gpu

@pytest.fixture
def mock_backend():
    backend = MagicMock()
    backend.is_available.return_value = True
    backend.supports_core_oc.return_value = True
    backend.supports_mem_oc.return_value = True
    backend.supports_voltage.return_value = True
    backend.name = "mock-backend"
    backend.priority = 99
    return backend

@pytest.fixture
def tmp_config(tmp_path):
    return str(tmp_path / "optimizer_config.json")
