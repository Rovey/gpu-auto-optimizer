"""Run gpu_optimizer with live terminal output and mirrored log file.

This preserves interactivity (colors + prompts) while writing a full transcript.
"""
from __future__ import annotations

import argparse
import io
import re
import runpy
import sys
import warnings
from pathlib import Path
from typing import Any


class TeeTextIO(io.TextIOBase):
    """Mirror writes to terminal stream and log file while preserving tty behavior."""

    def __init__(self, terminal_stream: Any, log_stream: io.TextIOBase) -> None:
        self._terminal = terminal_stream
        self._log = log_stream
        self._line_buffer = ""
        self._pending_cr_line: str | None = None
        self._last_progress_sig: str | None = None
        self._last_norm_line: str | None = None

    _ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
    _TIME_RE = re.compile(r"\b\d+:\d{2}:\d{2}\b")
    _PCT_RE = re.compile(r"\b\d{1,3}%\b")
    _SPINNER_RE = re.compile(r"^[\s\u2800-\u28FF\u25B6\u25B7\-]+")
    _SPINNER_CHARS = set("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏")

    def _strip_ansi(self, text: str) -> str:
        return self._ANSI_RE.sub("", text)

    def _is_progress_line(self, text: str) -> bool:
        s = self._strip_ansi(text).strip()
        if not s:
            return False
        return bool(self._PCT_RE.search(s) and self._TIME_RE.search(s))

    def _progress_signature(self, text: str) -> str:
        s = self._strip_ansi(text)
        # Keep only printable ASCII to strip spinner/progress glyph noise.
        s = re.sub(r"[^ -~]", " ", s)
        s = self._TIME_RE.sub("<time>", s)
        s = self._SPINNER_RE.sub("", s)
        s = re.sub(r"\s+", " ", s).strip()
        # Ignore precise timer value for dedup purposes.
        s = s.replace(" <time>", "")
        s = re.sub(r"\s+", " ", s).strip()
        return s

    def _is_dynamic_status_line(self, plain: str) -> bool:
        if self._TIME_RE.search(plain) and self._PCT_RE.search(plain):
            return True
        if any(ch in plain for ch in self._SPINNER_CHARS):
            return True
        if "━" in plain or "╺" in plain:
            return True
        return False

    def _emit_log_line(self, line: str) -> None:
        if self._log.closed:
            return

        plain = self._strip_ansi(line).strip()
        # Drop terminal-control-only lines (clear-line, cursor hide/show, etc.).
        if not plain:
            return

        if self._is_dynamic_status_line(plain):
            return

        if self._is_progress_line(line):
            # Progress redraws are highly repetitive and make logs unreadable.
            return

        self._last_progress_sig = None
        norm = re.sub(r"\s+", " ", plain)
        if norm and norm == self._last_norm_line:
            return
        self._last_norm_line = norm or None
        self._log.write(line + "\n")

    def _write_reduced_to_log(self, s: str) -> None:
        data = self._line_buffer + s
        parts = re.split(r"(\r|\n)", data)

        i = 0
        while i + 1 < len(parts):
            chunk = parts[i]
            sep = parts[i + 1]
            if sep == "\r":
                # Carriage return rewrites the same console row; keep only latest value.
                self._pending_cr_line = chunk
            elif chunk:
                self._pending_cr_line = None
                self._emit_log_line(chunk)
            elif self._pending_cr_line:
                self._emit_log_line(self._pending_cr_line)
                self._pending_cr_line = None
            else:
                self._log.write("\n")
                self._last_norm_line = None
                self._last_progress_sig = None
            i += 2

        self._line_buffer = parts[-1]

    def write(self, s: str) -> int:
        self._terminal.write(s)
        if not self._log.closed:
            self._write_reduced_to_log(s)
        return len(s)

    def flush(self) -> None:
        self._terminal.flush()
        if not self._log.closed:
            if self._line_buffer.strip():
                self._emit_log_line(self._line_buffer)
            elif self._pending_cr_line and self._pending_cr_line.strip():
                self._emit_log_line(self._pending_cr_line)
            self._line_buffer = ""
            self._pending_cr_line = None
            self._log.flush()

    def isatty(self) -> bool:
        return self._terminal.isatty()

    def fileno(self) -> int:
        return self._terminal.fileno()

    def __getattr__(self, name: str):
        return getattr(self._terminal, name)


def parse_args() -> tuple[Path, list[str]]:
    parser = argparse.ArgumentParser(
        description="Run gpu_optimizer with live output and write a log file.",
    )
    parser.add_argument("--log", required=True, help="Path to log file")
    parser.add_argument("args", nargs=argparse.REMAINDER, help="Arguments for gpu_optimizer")
    ns = parser.parse_args()

    args = ns.args
    if args and args[0] == "--":
        args = args[1:]

    return Path(ns.log), args


def main() -> int:
    log_path, app_args = parse_args()
    log_path.parent.mkdir(parents=True, exist_ok=True)

    warnings.filterwarnings("ignore", category=FutureWarning)

    original_stdout = sys.stdout
    original_stderr = sys.stderr

    with log_path.open("w", encoding="utf-8", newline="") as log_fh:
        tee_out = TeeTextIO(original_stdout, log_fh)
        tee_err = TeeTextIO(original_stderr, log_fh)
        sys.stdout = tee_out
        sys.stderr = tee_err

        # Make the optimizer think it's running directly as a script.
        script_path = Path(__file__).with_name("gpu_optimizer.py")
        sys.argv = [str(script_path), *app_args]

        try:
            runpy.run_path(str(script_path), run_name="__main__")
            return 0
        except SystemExit as exc:
            code = exc.code
            if isinstance(code, int):
                return code
            if code is None:
                return 0
            return 1
        except KeyboardInterrupt:
            sys.stderr.write("\nInterrupted (KeyboardInterrupt).\n")
            return 130
        finally:
            sys.stdout = original_stdout
            sys.stderr = original_stderr


if __name__ == "__main__":
    raise SystemExit(main())
