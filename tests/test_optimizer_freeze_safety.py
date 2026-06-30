"""Integration: the V/F undervolt search must avoid a voltage that previously
froze the PC (recorded as a hang in the journal). No hardware required."""
import threading
from types import SimpleNamespace

from src.backends.nvapi_vfcurve import NVAPIVFCurveBackend
from src.optimizer import GPUOptimizer
from src.search_journal import SearchJournal


class _FakeCurvePoint:
    def __init__(self, voltage_mv, base_freq_khz):
        self.voltage_mv = voltage_mv
        self.base_freq_khz = base_freq_khz


class _FakeVFBackend(NVAPIVFCurveBackend):
    """Subclass so isinstance() checks pass, but skip all hardware."""
    def __init__(self):
        self.applied_voltages = []

    def read_vf_curve(self, idx):
        # Flat curve 800..1000 mV, every point's base freq = 2400 MHz.
        return [_FakeCurvePoint(v, 2400 * 1000) for v in range(800, 1001, 25)]

    def apply_vf_lock(self, idx, target_voltage_uv, target_freq_khz):
        self.applied_voltages.append(target_voltage_uv // 1000)
        return True


def _metrics():
    return SimpleNamespace(core_clock_mhz=2400, boost_clock_mhz=2400,
                           gpu_util_pct=100, temp_c=60, power_w=180)


def _passing_stability():
    # High loaded clock so the clock-hold gate passes.
    snaps = [SimpleNamespace(core_clock_mhz=2400, gpu_util_pct=100) for _ in range(5)]
    return SimpleNamespace(passed=True, valid_load=True, snapshots=snaps)


def _make_optimizer(journal):
    opt = object.__new__(GPUOptimizer)          # bypass hardware __init__
    opt._gpu = SimpleNamespace(index=0, name="Fake", supports_uv=True)
    opt._backend = _FakeVFBackend()
    opt._monitor = SimpleNamespace(read_once=_metrics)
    opt._profile = {"voltage_min_mv": 800, "test_duration_sec": 30, "test_passes": 1}
    opt._journal = journal
    opt._progress = None
    opt._cancel_event = threading.Event()
    opt._stability_test_with_retries = lambda duration_sec: _passing_stability()
    return opt


def test_vf_search_avoids_a_previously_hung_voltage(tmp_path):
    journal = SearchJournal(tmp_path / "journal.json")
    # A prior run froze the PC at 850 mV (begun, never completed).
    journal.begin("vf_voltage", 850.0)

    opt = _make_optimizer(journal)
    opt._search_optimal_vf_point(_metrics())

    applied = opt._backend.applied_voltages
    assert applied, "search should have tried at least one voltage"
    # The hung voltage and anything lower (more aggressive) must never be applied.
    assert 850 not in applied
    assert min(applied) > 850

    # The voltages it DID try were completed (not left as new hangs).
    hung_after = SearchJournal(tmp_path / "journal.json").analyze().hung_values("vf_voltage")
    assert hung_after == [850.0]


def test_core_search_avoids_a_previously_hung_offset(tmp_path):
    journal = SearchJournal(tmp_path / "journal.json")
    journal.begin("core", 200.0)   # a prior run froze the PC at +200 MHz core

    applied = []
    opt = object.__new__(GPUOptimizer)
    opt._gpu = SimpleNamespace(index=0, name="Fake")
    opt._profile = {"core_offset_mhz_max": 300, "test_passes": 1, "test_duration_sec": 30}
    opt._monitor = SimpleNamespace(read_once=_metrics)
    opt._journal = journal
    opt._progress = None
    opt._cancel_event = threading.Event()
    opt._baseline_core_mhz = 2000
    # Working backend: the applied offset always reads back (models a power-limited card
    # where the load clock does NOT rise but the offset is genuinely applied).
    opt._backend = SimpleNamespace(
        verify=lambda idx: {"core_offset_khz": opt._core_offset_mhz * 1000}
    )

    def fake_apply():
        applied.append(opt._core_offset_mhz)
        return SimpleNamespace(success=True, verified=True, notes="")

    opt._apply = fake_apply
    opt._stability_test_with_retries = lambda duration_sec: _passing_stability()

    opt._binary_search_core()

    assert applied, "search should have tried at least one offset"
    # +200 and anything higher (more aggressive) must never be applied during search.
    assert all(o < 200 for o in applied)
