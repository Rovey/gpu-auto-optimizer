# GPU Auto Optimizer — P4c (Build Hygiene and Deferred Cleanup) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `/W4 /WX` clean build with hardening flags and a manifest, presets, an ASan build in CI, and the real deferred minors fixed.

**Spec:** `docs/superpowers/specs/2026-09-27-p4c-build-hygiene-and-cleanup-design.md`

## Global Constraints

- English only. `src/core/` has no Windows headers. Every behaviour change in `core` gets a failing test first.
- Warnings are fixed, not suppressed; a suppression needs a comment saying why.
- `cmake` lives in `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`.
- Push after every commit.

## Review Focus

1. A journal line with an id near `INT_MAX` must not make `begin()` return a negative id. (Task 3 test.)
2. `boot_strikes` of `3.0`, `-1`, `4294967296` or `"x"` must not re-enable boot-apply. (Task 3 test.)
3. A failed `journal.complete()` must stop the run at stock. (Task 3 test.)
4. `/WX` must not break the vendored third-party headers. (Task 1: SYSTEM includes + `/external:W0`.)
5. The ASan job must actually run `core_tests` under ASan, not skip silently. (Task 2: the job fails if the ASan runtime DLL is not found.)

---

### Task 1: Presets, warnings, hardening, manifest

**Files:** `CMakeLists.txt`, `CMakePresets.json` (new), `src/app/gao.manifest` (new), `src/app/main.cpp`, any file a new warning points at.

- [ ] Add `gao_warnings` (INTERFACE) with the spec's `/W4 /permissive- /w14…` list and `/external:W0 /external:anglebrackets`; link it into `core`, `hw`, `gao`, `core_tests`; set `COMPILE_WARNING_AS_ERROR ON` on those four; make every `third_party` include `SYSTEM`.
- [ ] `gao`: `target_compile_options(/guard:cf /sdl)`, `target_link_options(/GUARD:CF /CETCOMPAT)`.
- [ ] `src/app/gao.manifest`: asInvoker, supportedOS Win10/11, longPathAware, activeCodePage UTF-8, dpiAwareness PerMonitorV2; add to `gao`'s sources. `main()`: `SetConsoleOutputCP(CP_UTF8)` right after `SetDefaultDllDirectories`.
- [ ] `CMakePresets.json` (version 6): configure `default` (`build/`) and `asan` (`build-asan/`, `GAO_ASAN=ON`); build `debug`, `release`, `asan` (RelWithDebInfo); test presets mirroring them with `outputOnFailure`; workflow `ci`.
- [ ] Build Debug + Release; fix every warning. Verify with `dumpbin /headers build\Release\gao.exe` (Guard CF, CET compatible) and `mt -inputresource:build\Release\gao.exe;#1 -out:con`.
- [ ] Commit: `build: presets, /W4 /WX, hardening flags and an application manifest`.

### Task 2: ASan and CI

**Files:** `CMakeLists.txt`, `.github/workflows/ci.yml`, `.github/dependabot.yml` (new).

- [ ] `option(GAO_ASAN ...)`: when ON, `core` and `core_tests` get `/fsanitize=address` and link `/INCREMENTAL:NO`.
- [ ] Local: `cmake --preset asan`, `cmake --build --preset asan`, run `core_tests` with the MSVC `bin\Hostx64\x64` directory on `PATH`.
- [ ] CI: `runs-on: windows-2025`, `timeout-minutes: 20`, log `cmake --version`, `cmake --workflow --preset ci`; a second job `asan` that finds the MSVC tools dir with `vswhere`, prepends it to `PATH`, and runs the `asan` presets; fails if `clang_rt.asan_dynamic-x86_64.dll` is not found.
- [ ] `.github/dependabot.yml`: `github-actions`, weekly, `cooldown: default-days: 7`.
- [ ] Commit: `ci: pinned runner, presets workflow, ASan job, Dependabot`.

### Task 3: Deferred minors in core (TDD)

**Files:** `src/core/journal.cpp`, `src/core/config.cpp`, `src/core/search.cpp`, tests.

- [ ] Tests first:
  - journal ignores lines whose id is ≤ 0 or ≥ 1 000 000 000, and clock values outside 0..100 000;
  - `boot_strikes` of `3.0`, `-1`, `4294967296`, `"x"`, `1001` read as `kMaxBootStrikes` (fail closed); `0`–`1000` read as themselves; a missing field reads as 0;
  - `optimize()` stops at stock when `journal.complete` fails (fake journal sink that fails on `complete` lines).
- [ ] Implement; run; commit `fix(core): bound journal values, fail closed on bad strike counts, stop when a journal close fails`.

### Task 4: Deferred minors in hw/app

**Files:** `src/app/main.cpp`, `src/hw/gpu_control.cpp`, `src/hw/app_files.cpp`, `docs/hardware-checks.md`.

- [ ] `--stress`: drop the `errors=` column; VERDICT line uses `FormatField` (`n/a` instead of `-1`).
- [ ] `read_lines` → `std::optional<std::vector<std::string>>` (nullopt when the file exists but cannot be read); `--optimize` refuses without its journal.
- [ ] `--optimize` writes `{"session":"<time>"}` to the journal right after the baseline starts; failure stops before tuning.
- [ ] `power_limit_range_pct` callback returns `{0,0}` when the query fails.
- [ ] `--reset` prints `power limit: not restored (…)` and exits 1 when NVML fails.
- [ ] `--optimize` refuses to save a profile with an unknown driver or GPU id; saving resets strikes to 0.
- [ ] Unknown driver at logon: "driver version unknown (NVML); not applied".
- [ ] `write_file_atomic` deletes its temp file on failure.
- [ ] Hardware-checks rows 10 and 12 wording (steady-state power; exit code note).
- [ ] Build, smoke (`--stress 3`, `--status`, `--bandwidth`); commit `fix: deferred minors from the P2-P4 reviews`.

### Task 5: Review and merge

- [ ] Review package, fresh reviewer, one fix pass, merge to `main`, push.
