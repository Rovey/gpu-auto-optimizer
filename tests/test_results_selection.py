"""Results screen must show the CURRENT GPU's result, not a stale one from another
machine. Live bug: an old 'RTX 2060 SUPER' entry surfaced on an RTX 4070 because the
selector used values()[-1] (GPU-agnostic, order-fragile)."""
from src.gui.results import select_result_for_gpu


def test_prefers_current_gpu_over_other_entries():
    per = {
        "NVIDIA GeForce RTX 2060 SUPER": {"gpu_name": "NVIDIA GeForce RTX 2060 SUPER"},
        "NVIDIA GeForce RTX 4070":       {"gpu_name": "NVIDIA GeForce RTX 4070"},
    }
    r = select_result_for_gpu(per, "NVIDIA GeForce RTX 4070")
    assert r["gpu_name"] == "NVIDIA GeForce RTX 4070"


def test_prefers_current_gpu_even_when_not_last_inserted():
    # current GPU entry inserted FIRST; values()[-1] would wrongly pick the other one
    per = {
        "NVIDIA GeForce RTX 4070":       {"gpu_name": "NVIDIA GeForce RTX 4070"},
        "NVIDIA GeForce RTX 2060 SUPER": {"gpu_name": "NVIDIA GeForce RTX 2060 SUPER"},
    }
    r = select_result_for_gpu(per, "NVIDIA GeForce RTX 4070")
    assert r["gpu_name"] == "NVIDIA GeForce RTX 4070"


def test_fallback_to_most_recent_when_no_match():
    per = {"A": {"k": 1}, "B": {"k": 2}}
    assert select_result_for_gpu(per, "NVIDIA GeForce RTX 5090")["k"] == 2


def test_fallback_when_gpu_name_none():
    per = {"A": {"k": 1}, "B": {"k": 2}}
    assert select_result_for_gpu(per, None)["k"] == 2


def test_none_when_empty():
    assert select_result_for_gpu({}, "NVIDIA GeForce RTX 4070") is None
