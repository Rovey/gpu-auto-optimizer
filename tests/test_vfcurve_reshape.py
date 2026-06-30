"""Phase B: V/F curve RESHAPE undervolt (replaces the freeze-prone single-point lock).

Reshape contract:
  - Points BELOW the target voltage are left at stock (delta 0) so the GPU keeps
    scaling voltage/frequency dynamically under light load (no hard lock).
  - Points AT or ABOVE the target voltage are flattened so their effective frequency
    equals target_freq — a flat top that caps the frequency, which in turn caps the
    voltage the GPU will request (it won't raise voltage for zero frequency gain).
  - NO voltage lock is ever set. That hard lock is what froze the RTX 4070.
"""
import struct

from src.backends.nvapi_vfcurve import (
    VFPoint,
    NVAPIVFCurveBackend,
    _compute_reshape_deltas,
    _parse_vf_points,
    _ENTRIES_OFFSET,
    _MASK_ENTRY_SIZE,
    _VFP_ENTRY_SIZE,
    _MASK_BUF_SIZE,
    _VFP_BUF_SIZE,
    _CLOCK_TABLE_BUF_SIZE,
    _CLOCK_DOMAIN_GRAPHICS,
)


def _pack_buffers(points):
    """Build minimal valid (mask, vfp, ct) buffers for a list of (voltage_uv, base_freq_khz)."""
    mask = bytearray(_MASK_BUF_SIZE)
    vfp = bytearray(_VFP_BUF_SIZE)
    ct = bytearray(_CLOCK_TABLE_BUF_SIZE)
    for i, (uv, fkhz) in enumerate(points):
        m_off = _ENTRIES_OFFSET + i * _MASK_ENTRY_SIZE
        struct.pack_into('<I', mask, m_off, _CLOCK_DOMAIN_GRAPHICS)  # graphics domain
        mask[m_off + 4] = 1                                          # enabled
        v_off = _ENTRIES_OFFSET + i * _VFP_ENTRY_SIZE
        struct.pack_into('<I', vfp, v_off + 4, fkhz)                 # base freq khz
        struct.pack_into('<I', vfp, v_off + 8, uv)                  # voltage uv
        # ct delta left 0 (stock)
    return bytes(mask), bytes(vfp), bytes(ct)


# ---------------------------------------------------------------------------
# Pure function: _compute_reshape_deltas
# ---------------------------------------------------------------------------

def test_reshape_keeps_below_target_stock_flattens_above():
    pts = [
        VFPoint(850000, 2400000, 0),
        VFPoint(900000, 2550000, 0),
        VFPoint(950000, 2700000, 0),
        VFPoint(1000000, 2850000, 0),
    ]
    deltas = _compute_reshape_deltas(pts, target_voltage_uv=950000, target_freq_khz=2800000)
    # below cap → 0; at cap → +100 MHz; above cap → flattened down to target
    assert deltas == [0, 0, 100000, -50000]


def test_reshape_target_point_raised_to_target_freq():
    pts = [VFPoint(950000, 2700000, 0)]
    deltas = _compute_reshape_deltas(pts, target_voltage_uv=950000, target_freq_khz=2800000)
    assert deltas == [100000]


def test_reshape_leaves_below_cap_points_untouched():
    # A point below the cap voltage stays stock (delta 0) even if its stock freq is high —
    # dynamic boost below the cap is desired; capping happens only at/above target voltage.
    pts = [VFPoint(800000, 2900000, 0)]
    deltas = _compute_reshape_deltas(pts, target_voltage_uv=950000, target_freq_khz=2800000)
    assert deltas == [0]


# ---------------------------------------------------------------------------
# Backend method: apply_vf_reshape — must NOT set a hard voltage lock
# ---------------------------------------------------------------------------

def test_apply_reshape_writes_flat_top_and_sets_no_lock():
    pts = [(850000, 2400000), (900000, 2550000), (950000, 2700000), (1000000, 2850000)]
    mask, vfp, ct = _pack_buffers(pts)

    b = object.__new__(NVAPIVFCurveBackend)
    b._read_raw_mask = lambda gi: mask
    b._read_raw_vfp = lambda gi, m=None: vfp
    b._read_raw_clock_table = lambda gi, m=None: ct
    written = {}
    b._write_raw_clock_table = lambda gi, buf: (written.__setitem__('buf', buf), True)[1]
    set_calls = []
    clear_calls = []
    b._set_voltage_lock = lambda gi, uv: set_calls.append(uv) or True
    b._clear_voltage_lock = lambda gi: clear_calls.append(True) or True

    ok = b.apply_vf_reshape(0, target_voltage_uv=950000, target_freq_khz=2800000)

    assert ok is True
    assert set_calls == []          # KEY safety property: reshape never sets a hard lock
    assert clear_calls == [True]    # and it clears any stale lock so none remains

    pts_after = _parse_vf_points(mask, vfp, written['buf'])
    eff = {p.voltage_uv: p.effective_freq_mhz for p in pts_after}
    assert eff[850000] == 2400      # below cap: stock preserved (dynamic)
    assert eff[900000] == 2550      # below cap: stock preserved
    assert eff[950000] == 2800      # at cap: raised to target
    assert eff[1000000] == 2800     # above cap: flattened down to target


def _fake_pynvml():
    import types
    m = types.ModuleType("pynvml")
    m.nvmlInit = lambda: None
    m.nvmlShutdown = lambda: None
    m.nvmlDeviceGetHandleByIndex = lambda i: object()
    m.nvmlDeviceGetPowerManagementDefaultLimit = lambda h: 200000
    m.nvmlDeviceGetPowerManagementLimitConstraints = lambda h: (100000, 250000)
    m.nvmlDeviceSetPowerManagementLimit = lambda h, mw: None
    return m


def test_apply_routes_to_reshape_not_lock(monkeypatch):
    import sys
    monkeypatch.setitem(sys.modules, "pynvml", _fake_pynvml())

    b = object.__new__(NVAPIVFCurveBackend)
    b.is_available = lambda: True
    calls = []
    b.apply_vf_reshape = (
        lambda gi, target_voltage_uv, target_freq_khz:
        calls.append(("reshape", target_voltage_uv, target_freq_khz)) or True
    )
    b.apply_vf_lock = lambda *a, **k: calls.append(("lock",) + a) or True

    res = b.apply(0, target_voltage_mv=950, target_freq_mhz=2800, power_limit_pct=100)

    assert ("reshape", 950000, 2800000) in calls   # routes through reshape
    assert all(c[0] != "lock" for c in calls)       # never through the freeze-prone lock
    assert res.success is True
    assert res.target_voltage_mv == 950
    assert res.target_freq_mhz == 2800


def test_reset_returns_true_when_readback_is_stock_despite_false_write_rc(monkeypatch):
    """Some drivers return a non-OK rc from SetClockBoostTable even when the write
    applied (seen live: reset() reported False but the curve was actually stock).
    reset() must trust the read-back curve state, not the write's return code."""
    import types
    import src.backends.nvapi as nvapi_mod

    pts = [(850000, 2400000), (900000, 2550000), (950000, 2700000)]
    mask, vfp, ct = _pack_buffers(pts)
    state = {"ct": ct}

    b = object.__new__(NVAPIVFCurveBackend)
    b._read_raw_mask = lambda gi: mask
    b._read_raw_vfp = lambda gi, m=None: vfp
    b._read_raw_clock_table = lambda gi, m=None: state["ct"]

    def _write(gi, buf):
        state["ct"] = buf      # the write actually applies...
        return False           # ...but the driver reports a misleading non-OK rc

    b._write_raw_clock_table = _write
    b._clear_voltage_lock = lambda gi: True

    fake_loader = types.SimpleNamespace(set_pstate20_raw=lambda *a, **k: True)
    monkeypatch.setattr(nvapi_mod._NVAPILoader, "get", lambda: fake_loader)

    assert b.reset(0) is True

