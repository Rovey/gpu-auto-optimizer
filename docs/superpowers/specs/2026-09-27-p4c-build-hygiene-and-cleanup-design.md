# P4c — Build Hygiene and Deferred Cleanup

Date: 2026-09-27
Status: approved (design) under the user's overnight mandate ("pick the clearly best option,
don't block on questions"); not yet implemented
Source: best-practice research of 2026-09-27 (items 9–12) and the minor findings deferred by the
P2, P3 and P4 reviews.

## 1. Goal

Cheap, high-value build and CI hygiene before the UI work, plus the deferred minors that are
real (not cosmetic) — so P5 starts from a codebase that compiles warning-clean at `/W4`, is
hardened as an elevated binary, is sanitizer-checked, and has no known silent failure paths.

## 2. Build

1. **`CMakePresets.json`** (schema 6, CMake ≥ 3.25; the project requires 3.28):
   - configure `default` (Visual Studio 18 2026 generator, x64, `build/`) and `asan`
     (`build-asan/`, `GAO_ASAN=ON`);
   - build presets `debug`, `release`, `asan` (RelWithDebInfo);
   - test presets with `outputOnFailure`;
   - workflow preset `ci` = configure `default` → build `release` → test `release`.
2. **Warnings, per target through an interface library `gao_warnings`:**
   - `/W4 /permissive- /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296 /w14311
     /w14545 /w14546 /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928`
     (the cpp-best-practices MSVC set);
   - `COMPILE_WARNING_AS_ERROR ON` for `core`, `hw`, `gao`, `core_tests`;
   - `third_party/` becomes a `SYSTEM` include directory; `/external:W0` keeps doctest and
     nlohmann/json quiet;
   - every warning this surfaces is fixed, not suppressed (a suppression needs a comment
     saying why).
3. **Hardening for the elevated exe (`gao` only):** compile `/guard:cf /sdl`, link `/GUARD:CF
   /CETCOMPAT` (x64; `/DYNAMICBASE` and `/HIGHENTROPYVA` are already MSVC's x64 defaults).
4. **Application manifest** (`src/app/gao.manifest`, embedded): `requestedExecutionLevel
   asInvoker` (explicit — elevation is always the user's choice), Windows 10/11
   `supportedOS`, `longPathAware`, `activeCodePage UTF-8`, and `dpiAwareness PerMonitorV2`
   (needed for P5, harmless for the console). `main` sets the console output code page to
   UTF-8 so paths print correctly.
5. **ASan:** `GAO_ASAN=ON` adds `/fsanitize=address` and `/INCREMENTAL:NO` to `core` and
   `core_tests` (hardware code gains little: NVML/NVAPI are not instrumented). Runs in CI as its
   own job.

## 3. CI

- `runs-on: windows-2025` (pinned instead of the moving `windows-latest` label), `timeout-minutes:
  20`, `cmake --version` logged, build via `cmake --workflow --preset ci`.
- ASan job: configure/build/test the `asan` presets; the MSVC ASan runtime DLL directory is put on
  `PATH` from `vswhere`.
- `.github/dependabot.yml` for `github-actions`, weekly, with a 7-day cooldown.
- Not done (research: low value here): caching, harden-runner, Scorecard, SBOM, zizmor in CI
  (can be run locally).

## 4. Deferred minors fixed in this round

| From | Finding | Fix |
|---|---|---|
| P2 | `errors=` column in `--stress` can never show a non-zero value | drop the column; the verdict line already says WRONG RESULT |
| P2 | VERDICT line prints `-1` instead of `n/a` | use `FormatField` |
| P3 | `journal.complete()` result ignored | a failed `complete` stops the run (the value would otherwise be blacklisted silently) |
| P3 | an existing but unreadable journal is read as empty (ceilings lost) | `read_lines` reports failure; `--optimize` refuses to run without its journal |
| P3 | a journal that cannot be written is found only at the core search | a `{"session":…}` line is written right after the baseline starts; failure stops the run before any tuning |
| P3 | `*nvml.PowerLimitRangePct()` deref is UB if the second query fails | the callback returns `{0, 0}` on failure (the power step already skips such a range) |
| P3 | hand-edited INT_MIN/INT_MAX journal values overflow `ceiling - 1` / `next_id` | the parser ignores ids ≤ 0 or ≥ 1 000 000 000 and clock values outside 0..100 000 |
| P3 | `--reset` silently skips power when NVML fails | print `power limit: not restored (NVML: …)` and exit 1 |
| P4 | fractional or overflowing `boot_strikes` read as 0 (fail-open) | anything that is not an integer in 0..1000 reads as `kMaxBootStrikes` (fail-closed) |
| P4 | `--optimize` saves a profile when the driver version is unknown | refuse to save, say why |
| P4 | an unknown driver at logon is reported as "driver changed" | its own message: "driver version unknown (NVML); not applied" |
| P4 | a new profile inherits the previous profile's strikes | saving a profile resets strikes to 0 |
| P4 | failed atomic write leaves `gao.json.tmp` | delete the temp file on failure |

Left deferred (documented, not worth the complexity now): the microsecond race between
boot-apply's strike reset and an `--optimize` save; `--status` reporting "off" when `schtasks`
itself fails; strikes counted for logoff/reboot within 2 minutes.

## 5. Testing

Every fixed minor that lives in `core` gets a failing test first (journal id/value bounds,
strike fail-closed, complete-failure stops the run, profile save resets strikes where it is core
logic). Warnings/hardening/manifest/presets are verified by a clean `/W4 /WX` build, `dumpbin
/headers` (Guard CF, CET), the embedded manifest (`mt -inputresource`), and green CI including the
ASan job.
