# tests/test_monitor.py
from unittest.mock import MagicMock, patch
from src.monitor import GPUMetrics, sample_average_under_load

def test_filters_out_idle_samples():
    """Only samples with gpu_util >= threshold should be included."""
    monitor = MagicMock()
    samples = [
        GPUMetrics(gpu_index=0, gpu_util_pct=10, core_clock_mhz=300),
        GPUMetrics(gpu_index=0, gpu_util_pct=95, core_clock_mhz=1800),
        GPUMetrics(gpu_index=0, gpu_util_pct=99, core_clock_mhz=1820),
    ]
    call_count = 0
    def read_side_effect():
        nonlocal call_count
        if call_count < len(samples):
            s = samples[call_count]
            call_count += 1
            return s
        return samples[-1]
    monitor.read_once.side_effect = read_side_effect
    monitor._index = 0

    with patch("src.monitor.time") as mock_time:
        # deadline = time.time() + 1.0 => 0.0 + 1.0 = 1.0
        # loop check: 0.1 < 1.0 -> collect sample 1, sleep
        # loop check: 0.5 < 1.0 -> collect sample 2, sleep
        # loop check: 0.8 < 1.0 -> collect sample 3, sleep
        # loop check: 1.5 >= 1.0 -> exit
        mock_time.time.side_effect = [0.0, 0.1, 0.5, 0.8, 1.5]
        mock_time.sleep = MagicMock()
        result = sample_average_under_load(monitor, duration_sec=1.0, min_util_pct=80)
    assert result.core_clock_mhz == 1810  # avg of 1800 and 1820
    assert result.samples_used == 2

def test_returns_all_samples_if_none_qualify():
    """If no samples meet threshold, return raw average with samples_used=0."""
    monitor = MagicMock()
    samples = [
        GPUMetrics(gpu_index=0, gpu_util_pct=10, core_clock_mhz=300),
        GPUMetrics(gpu_index=0, gpu_util_pct=5, core_clock_mhz=280),
    ]
    call_count = 0
    def read_side_effect():
        nonlocal call_count
        if call_count < len(samples):
            s = samples[call_count]
            call_count += 1
            return s
        return samples[-1]
    monitor.read_once.side_effect = read_side_effect
    monitor._index = 0

    with patch("src.monitor.time") as mock_time:
        mock_time.time.side_effect = [0.0, 0.5, 1.0]
        mock_time.sleep = MagicMock()
        result = sample_average_under_load(monitor, duration_sec=0.5, min_util_pct=80)
    assert result.samples_used == 0
