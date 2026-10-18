# GPU Auto Optimizer

Instructions for AI coding agents working in this repository. A native C++20 Windows tool that tunes
NVIDIA GPUs (power limit, core and memory offsets) through NVML and NVAPI, with a DX11 stress test.
User-facing overview: `README.md`. Code layout, profiles and release process: `docs/development.md`.

## Hard requirement: English only

Everything in this repository is in English, without exception: code, identifiers, comments,
CLI output and log lines, commit messages, branch names, PR titles and descriptions, specs,
plans, README, `docs/`, test names and file names. This holds even when the conversation with
the user is in another language — translate, never paste non-English text into the project.

## Build and test

`cmake` is not on `PATH`; use the one bundled with Visual Studio:

```powershell
$bin = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$bin\cmake.exe" --workflow --preset ci          # configure, build Release, test
& "$bin\cmake.exe" --build --preset debug; & "$bin\ctest.exe" --preset debug
```

Everything we compile is `/W4 /WX`; fix warnings, do not suppress them.

## Hardware tests: targeted first

A full All in one run takes about 40 minutes (five 5-minute measurements). To check a change on a card,
test the part that changed, directly; it usually takes minutes:

- Flat top / undervolt write: `gao --curve-flatten <mV> <MHz> [<drop MHz>] [<core MHz>]` during and right
  after `gao --stress <s>` (warm and cooling card), then `gao --apply` to put the saved tune back.
- Watchdog and recovery: the tray app running, `dxcap -forcetdr` (elevated), with or without
  `gao --stress` running; read `boot.log` and `gao --status`.
- Fans: `gao --stress <s>` and `gao --status` every few seconds.
- What a run logs: an aborted or refused run already writes its `RESULT:` line to `run.log`.

Before anything that can save a profile, copy `%ProgramData%\GpuAutoOptimizer\gao.json`: a run replaces the
saved tune as soon as its overclock step ends. Keep full runs for release checks, when nobody needs the
machine; run them from the command line with the window closed, because the NVIDIA app's Instant Replay
records while the app's window is open and lowers the measurements by about 2 %. `tools\hwchecks.ps1`
drives the checks of `docs/hardware-checks.md` that run `gao`.

## Layout

Two executables on one engine: `gao.exe` (CLI, `src/app/main.cpp`) and
`GpuAutoOptimizer.exe` (window + tray + watchdog, `src/app/gui/`). Logic both
need lives in `src/app/common.*`, never in either entry point.

## Rules the design depends on

- `src/core/` is pure logic: no `windows.h`, NVML, NVAPI or D3D headers. It reaches hardware
  only through `GpuControl` and callbacks, so CI tests it without a GPU.
- Every hardware write is verified by reading it back; a mismatch is a failure. Any failed
  apply ends at stock.
- The crash journal's `begin` line is flushed to disk before a clock candidate touches the
  hardware.
- `src/hw/` is verified by hand on the reference RTX 4070 via `docs/hardware-checks.md`; record
  the results there.
- Commands that write to the GPU need an elevated shell.
