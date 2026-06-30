"""The single-point V/F-curve lock hard-freezes some GPUs (seen on RTX 4070 /
driver 610), so the V/F-curve backend is excluded from selection by default —
Balanced/Performance fall back to the stable PState20 core/mem OC path until a
curve-reshape undervolt (Phase B) replaces the lock."""
import src.optimizer as opt_mod
from src.backends.nvapi_vfcurve import NVAPIVFCurveBackend


class _FakePState20:
    priority = 30
    def is_available(self):
        return True


def test_vf_curve_backend_excluded_by_default():
    vf = object.__new__(NVAPIVFCurveBackend)   # isinstance check target, no hardware
    vf.is_available = lambda: True
    vf.priority = 100                           # higher priority, but must be skipped
    p20 = _FakePState20()

    assert opt_mod.ENABLE_VF_CURVE_UNDERVOLT is False
    chosen = opt_mod._best_backend(None, candidates=[vf, p20])
    assert chosen is p20


def test_vf_curve_backend_used_when_explicitly_enabled(monkeypatch):
    monkeypatch.setattr(opt_mod, "ENABLE_VF_CURVE_UNDERVOLT", True)
    vf = object.__new__(NVAPIVFCurveBackend)
    vf.is_available = lambda: True
    vf.priority = 100
    p20 = _FakePState20()

    chosen = opt_mod._best_backend(None, candidates=[vf, p20])
    assert chosen is vf
