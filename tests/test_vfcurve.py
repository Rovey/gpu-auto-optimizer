"""Tests for V/F curve backend data structures and parsing."""
import struct
import pytest


def test_vfpoint_creation():
    from src.backends.nvapi_vfcurve import VFPoint
    p = VFPoint(voltage_uv=900000, base_freq_khz=1800000, delta_khz=50000)
    assert p.voltage_uv == 900000
    assert p.base_freq_khz == 1800000
    assert p.delta_khz == 50000
    assert p.voltage_mv == 900
    assert p.effective_freq_mhz == 1850


def test_vfpoint_effective_freq():
    from src.backends.nvapi_vfcurve import VFPoint
    p = VFPoint(voltage_uv=850000, base_freq_khz=1700000, delta_khz=-100000)
    assert p.effective_freq_mhz == 1600


def test_parse_vf_points_from_raw_buffers():
    """Simulate parsing raw mask + VFP + clock table buffers."""
    from src.backends.nvapi_vfcurve import _parse_vf_points, _MASK_ENTRY_SIZE, _VFP_ENTRY_SIZE, _CLOCK_TABLE_ENTRY_SIZE

    # Build minimal fake buffers with 2 graphics points
    # Mask buffer: version(4) + mask(64) + entries(255 * 24)
    mask_buf = bytearray(6188)
    struct.pack_into('<I', mask_buf, 0, 0x0001182C)  # version
    # Entry 0: clockType=0 (graphics), enabled=1
    off = 68
    struct.pack_into('<I', mask_buf, off, 0)  # clockType = graphics
    mask_buf[off + 4] = 1  # enabled
    # Entry 1: clockType=0 (graphics), enabled=1
    off = 68 + _MASK_ENTRY_SIZE
    struct.pack_into('<I', mask_buf, off, 0)
    mask_buf[off + 4] = 1

    # VFP buffer: version(4) + mask(64) + entries(255 * 28)
    vfp_buf = bytearray(7208)
    struct.pack_into('<I', vfp_buf, 0, 0x00011C28)
    # Entry 0: 850mV, 1700 MHz
    off = 68
    struct.pack_into('<I', vfp_buf, off, 0)        # clockType
    struct.pack_into('<I', vfp_buf, off + 4, 1700000)  # freq kHz
    struct.pack_into('<I', vfp_buf, off + 8, 850000)   # voltage uV
    # Entry 1: 900mV, 1800 MHz
    off = 68 + _VFP_ENTRY_SIZE
    struct.pack_into('<I', vfp_buf, off, 0)
    struct.pack_into('<I', vfp_buf, off + 4, 1800000)
    struct.pack_into('<I', vfp_buf, off + 8, 900000)

    # Clock table buffer: version(4) + mask(64) + entries(255 * 36)
    ct_buf = bytearray(9248)
    struct.pack_into('<I', ct_buf, 0, 0x00012420)
    # Entry 0: delta = 0
    off = 68
    struct.pack_into('<I', ct_buf, off, 0)       # clockType
    struct.pack_into('<i', ct_buf, off + 20, 0)  # delta (at 2x scale)
    # Entry 1: delta = +100 MHz (stored as 200000 kHz at 2x scale)
    off = 68 + _CLOCK_TABLE_ENTRY_SIZE
    struct.pack_into('<I', ct_buf, off, 0)
    struct.pack_into('<i', ct_buf, off + 20, 200000)

    points = _parse_vf_points(bytes(mask_buf), bytes(vfp_buf), bytes(ct_buf))
    assert len(points) == 2
    assert points[0].voltage_uv == 850000
    assert points[0].base_freq_khz == 1700000
    assert points[0].delta_khz == 0   # 0 / 2 = 0
    assert points[1].voltage_uv == 900000
    assert points[1].delta_khz == 100000  # 200000 / 2 = 100000


def test_2x_scaling_write():
    """Delta values must be multiplied by 2 when writing."""
    from src.backends.nvapi_vfcurve import _apply_delta_to_clock_table, _CLOCK_TABLE_ENTRY_SIZE
    ct_buf = bytearray(9248)
    struct.pack_into('<I', ct_buf, 0, 0x00012420)
    # Set up entry 0 as graphics
    off = 68
    struct.pack_into('<I', ct_buf, off, 0)  # clockType = graphics

    # Apply delta of +50000 kHz to entry index 0
    _apply_delta_to_clock_table(ct_buf, index=0, delta_khz=50000)

    # Read back: should be stored as 100000 (2x)
    stored = struct.unpack_from('<i', ct_buf, off + 20)[0]
    assert stored == 100000


def test_build_undervolt_deltas():
    """Given target voltage + freq, compute correct deltas for all points."""
    from src.backends.nvapi_vfcurve import VFPoint, _compute_undervolt_deltas

    points = [
        VFPoint(voltage_uv=800000, base_freq_khz=1600000, delta_khz=0),
        VFPoint(voltage_uv=850000, base_freq_khz=1700000, delta_khz=0),
        VFPoint(voltage_uv=900000, base_freq_khz=1800000, delta_khz=0),
        VFPoint(voltage_uv=950000, base_freq_khz=1900000, delta_khz=0),
        VFPoint(voltage_uv=1000000, base_freq_khz=2000000, delta_khz=0),
    ]

    # Target: 1850 MHz @ 900mV
    deltas = _compute_undervolt_deltas(points, target_voltage_uv=900000, target_freq_khz=1850000)

    # Points at or below 900mV: delta should make effective freq = 1850 MHz
    assert deltas[0] == 1850000 - 1600000  # +250 MHz
    assert deltas[1] == 1850000 - 1700000  # +150 MHz
    assert deltas[2] == 1850000 - 1800000  # +50 MHz

    # Points above 900mV: flatten to target_freq or below
    assert deltas[3] <= 1850000 - 1900000  # -50 MHz or lower
    assert deltas[4] <= 1850000 - 2000000  # -150 MHz or lower
