# GPU Auto Optimizer

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
