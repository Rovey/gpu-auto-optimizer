"""Crash-safe optimization journal (Phase A freeze-safety).

A write-ahead journal so the OC/undervolt search survives a hard system freeze.
Before each risky apply, the candidate setting is journalled to disk with state
"applying". After its stability test completes, the entry is updated to "tested".
If the process dies (PC freeze) between those two writes, the entry stays
"applying" — and on the next launch the journal reports that setting as a *hang*,
so the search can blacklist it and stay safer.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional


@dataclass
class JournalAnalysis:
    entries: List[dict]

    def is_hung(self, kind: str, value: float) -> bool:
        """True if this exact (kind, value) was applied but never completed."""
        return any(
            e.get("kind") == kind and e.get("value") == value and e.get("state") == "applying"
            for e in self.entries
        )

    def hung_values(self, kind: str) -> List[float]:
        """All values of `kind` that were applied but never completed (hangs).
        The search uses these as bounds: never retry a hung value or anything
        more aggressive than it (direction is the caller's concern)."""
        return [
            e.get("value") for e in self.entries
            if e.get("kind") == kind and e.get("state") == "applying"
        ]

    def last_good(self, kind: str) -> Optional[float]:
        """Value of the most recent step of `kind` that completed and passed."""
        for e in reversed(self.entries):
            if e.get("kind") == kind and e.get("state") == "tested" and e.get("passed"):
                return e.get("value")
        return None


class SearchJournal:
    def __init__(self, path) -> None:
        self._path = Path(path)

    def _load(self) -> List[dict]:
        if not self._path.exists():
            return []
        try:
            return json.loads(self._path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            return []

    def _save(self, entries: List[dict]) -> None:
        # Durable write: the "applying" record MUST reach the platter before the
        # risky apply runs, otherwise a freeze could lose it and we'd never learn
        # which setting hung. fsync forces it past the OS cache.
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with open(self._path, "w", encoding="utf-8") as f:
            f.write(json.dumps(entries, indent=2))
            f.flush()
            os.fsync(f.fileno())

    def clear(self) -> None:
        """Wipe the journal — call after a fully successful run so old hangs
        don't haunt future searches forever."""
        if self._path.exists():
            self._path.unlink()

    def begin(self, kind: str, value: float, extra: Optional[dict] = None) -> int:
        entries = self._load()
        seq = max((e.get("seq", 0) for e in entries), default=0) + 1
        entries.append({"seq": seq, "kind": kind, "value": value, "state": "applying"})
        self._save(entries)
        return seq

    def complete(self, seq: int, passed: bool, note: str = "") -> None:
        entries = self._load()
        for e in entries:
            if e.get("seq") == seq:
                e["state"] = "tested"
                e["passed"] = bool(passed)
                if note:
                    e["note"] = note
                break
        self._save(entries)

    def analyze(self) -> JournalAnalysis:
        return JournalAnalysis(self._load())
