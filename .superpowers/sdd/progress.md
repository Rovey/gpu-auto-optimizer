# Phase 1 — Afterburner Control Spike — Progress Ledger

Plan: docs/superpowers/plans/2026-06-29-phase1-afterburner-control-spike.md
Branch: cpp-afterburner-rewrite
Build/test: powershell -ExecutionPolicy Bypass -File cpp\build.ps1

- Task 0: BLOCKED on user — install MSI Afterburner + save Profile 5 (hardware tasks 6-8 gated on this)
- Task 1: complete (skeleton: CMake + vendored doctest + build.ps1; smoke test green) — done by controller
- Task 2: complete (commit cd2204a, controller review clean)
- Task 3: complete (commit 64b1747, synthetic round-trip green, real-fixture guarded)
- Task 4: complete (commit f112586, 3/3 pass)
- Task 5: complete (commit 852db52, 4/4 pass)
- Logic half (T2-5) verified green together at 852db52; deep review deferred to end-of-phase whole-branch review
- Task 6: complete (commit cf8bb18, NVML probe real 4070: limit=200W temp=34C — verified)
- Task 7: complete (commit adfe5af, Afterburner controller compiles clean)
- Task 8: harness built (commit 8ffd109). AB IS installed; FindAfterburner resolved 4070 profile cfg. Non-elevated run failed at backup (Program Files needs admin). Running elevated spike now.
- SPIKE RESULT: FAIL. Both AB triggers failed to apply: -Profile5 (no saved slot) and kill+relaunch (no startup-apply). Editing profile cfg files does not change hardware. AB orchestration not viable without UI automation / pre-config. GATE: reconsider control plane (recommend pivot to direct NVAPI).
- PIVOT VALIDATED: direct NVAPI control WORKS on RTX 4070 / driver 610 (Python backend). apply core+100MHz -> verify reads back 100000 kHz; reset clean. NVML power-limit set also succeeded. Control plane = NVAPI. AB dropped.
- PYTHON FIX: venv + cupy-cuda12x + nvidia cu12 runtime wheels pinned in requirements; start.bat/setup.bat added. Verified: cupy matmul OK, StabilityTester valid_load 95%, full Safe optimize end-to-end stable, GUI builds. App functional.
- PHASE A (freeze-safety) PART 1: crash-safe SearchJournal (src/search_journal.py, 5 tests) + wired into V/F undervolt search (begin/complete + hung-voltage avoidance + recovery warning) + clock-hold gate fix. Integration test proves hung-voltage avoidance. Full suite 54 passed. NOT yet: core/mem OC journaling, live hardware freeze-recovery verification, Phase B (curve reshape vs lock).
- PHASE A PART 2: journaling extended to core + mem OC searches (begin/complete + hung-offset ceiling); core integration test added. Suite 55 passed. Committed b2e837b.
- LIVE DEMO (real 4070, seeded 850mV hang): recovery DETECTION confirmed on hardware (optimizer saw hung [850.0]). BUT search crashed on a separate latent bug: NVML access violation (OSError) in monitor._read_nvml — cached handle invalidated by another component's nvmlShutdown; _read_nvml only catches NVMLError not OSError. GPU reset to stock + journal cleared after. NVML lifecycle bug = next task.
- NVML BUG FIXED: monitor._read_nvml re-acquires handle on OSError (2 tests). Suite 59.
- LIVE V/F demo: avoided seeded 850mV + NVML fix held, but V/F lock froze PC again at 900-925mV (session torn down). GPU left stock.
- SAFETY GATE: V/F-curve lock disabled by default (ENABLE_VF_CURVE_UNDERVOLT=False). Balanced/Perf now use stable PState20 (NVAPIBackend) core/mem OC + power. Real backend selection on 4070 confirmed = NVAPIBackend. Committed 8ec8de1.
- STATE: app safe + working. Stop live OC testing (kept freezing PC). TODO: Phase B curve-reshape undervolt; merge to main; README; prune debug files.
- PHASE B (curve-reshape undervolt): added `_compute_reshape_deltas` + `apply_vf_reshape` (nvapi_vfcurve.py). Reshape = flat-top curve at target_freq for V >= target, leave below-cap points stock, NO hard voltage lock (clears stale lock). apply() now routes to reshape not lock. TDD: 5 tests (below-cap stock / at-cap raised / above-cap flattened / no lock ever set / apply routing). Suite 64 passed. STILL GATED off (ENABLE_VF_CURVE_UNDERVOLT=False) pending supervised live test — reshape is the freeze-safe theory but NOT yet hardware-validated. Committed b3c1f44.
- CLEANUP: README rewritten (venv flow, NVAPI control plane, freeze-safety, undervolt status); pruned debug files (apply_and_verify.py, verify_output.txt). Committed f0cb476.
- MERGE: cpp-afterburner-rewrite -> main (fast-forward; brings shelved C++ under cpp/ along, isolated). Python app now on main.
- LIVE Balanced run on 4070 surfaced 2 GUI/optimizer bugs (both fixed + pushed):
  (1) Results screen showed a STALE cross-machine result (old RTX 2060 SUPER) — load_from_config used values()[-1] (GPU-agnostic) + completion never pushed the fresh result. Fixed: select_result_for_gpu(by current GPU) + _on_complete pushes result + stale entry purged. (commit af86bd7)
  (2) Core OC search abandoned at +0: it judged "did offset apply?" by under-load clock rise, which can't happen on a power-limited card (199W wall) -> false "backend failed" -> bail. Fixed: _core_offset_applied() judges by backend.verify() read-back. (commit 5a4b45e)
  Real 4070 Balanced result was mem+700, power+15%, boost 2520->2546, core+0 (the bug). Undervolt correctly skipped (gated). Suite 75 passed.
