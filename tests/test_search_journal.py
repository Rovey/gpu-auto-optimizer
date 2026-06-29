"""Tests for the crash-safe optimization journal (Phase A freeze-safety)."""
from src.search_journal import SearchJournal


def test_uncompleted_step_is_detected_as_hang(tmp_path):
    """A step that was begun but never completed (system froze mid-test)
    must be reported as a hang by a fresh journal reading the same file."""
    path = tmp_path / "journal.json"

    j = SearchJournal(path)
    j.begin("vf_voltage", 870.0)   # simulate: apply happened, then the PC froze
    # never call complete()

    # Fresh instance (as if after reboot) reads the persisted file
    recovered = SearchJournal(path)
    analysis = recovered.analyze()

    assert analysis.is_hung("vf_voltage", 870.0)


def test_completed_passing_step_is_not_hung_and_is_last_good(tmp_path):
    path = tmp_path / "journal.json"
    j = SearchJournal(path)
    seq = j.begin("core", 150.0)
    j.complete(seq, passed=True)

    analysis = SearchJournal(path).analyze()
    assert not analysis.is_hung("core", 150.0)
    assert analysis.last_good("core") == 150.0


def test_failed_step_is_neither_hung_nor_good(tmp_path):
    path = tmp_path / "journal.json"
    j = SearchJournal(path)
    seq = j.begin("core", 200.0)
    j.complete(seq, passed=False)   # unstable but did NOT freeze (test returned)

    analysis = SearchJournal(path).analyze()
    assert not analysis.is_hung("core", 200.0)
    assert analysis.last_good("core") is None


def test_hung_values_lists_only_uncompleted_steps(tmp_path):
    path = tmp_path / "journal.json"
    j = SearchJournal(path)
    s1 = j.begin("vf_voltage", 900.0)
    j.complete(s1, passed=True)        # safe
    j.begin("vf_voltage", 850.0)       # froze (never completed)
    j.begin("vf_voltage", 870.0)       # also recorded applying before freeze

    analysis = SearchJournal(path).analyze()
    assert sorted(analysis.hung_values("vf_voltage")) == [850.0, 870.0]
    assert analysis.hung_values("core") == []


def test_clear_removes_all_entries(tmp_path):
    path = tmp_path / "journal.json"
    j = SearchJournal(path)
    j.begin("core", 100.0)
    j.clear()
    assert SearchJournal(path).analyze().hung_values("core") == []
