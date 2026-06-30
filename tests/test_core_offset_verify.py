"""Core OC search must judge "did the offset apply?" by backend READ-BACK, not by
whether the under-load clock rose. On a power-limited GPU the load clock can't rise
even when the offset is correctly applied — the old heuristic mistook that for a
backend failure and abandoned the search at +0 (the live RTX 4070 symptom)."""
from src.optimizer import _core_offset_applied


class _Backend:
    def __init__(self, readback_khz):
        self._rb = readback_khz

    def verify(self, gpu_index):
        if self._rb is None:
            return None
        return {"core_offset_khz": self._rb}


def test_applied_true_when_readback_matches_even_if_load_clock_flat():
    # power-limited card: offset reads back as applied -> must be treated as applied
    assert _core_offset_applied(_Backend(100_000), 0, 100) is True


def test_applied_false_when_readback_is_zero():
    # genuine silent backend failure: offset did not stick
    assert _core_offset_applied(_Backend(0), 0, 100) is False


def test_applied_true_for_zero_offset():
    assert _core_offset_applied(_Backend(0), 0, 0) is True


def test_applied_true_when_verify_unavailable():
    class NoVerify:
        pass
    assert _core_offset_applied(NoVerify(), 0, 100) is True


def test_applied_true_when_verify_returns_none():
    assert _core_offset_applied(_Backend(None), 0, 100) is True
