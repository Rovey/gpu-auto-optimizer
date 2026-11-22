# Handover

Everything a developer or coding agent needs to continue this project on another computer, without the
previous machine, its local files or any conversation history. State as of **2026-11-22**, after the v0.4.10 release.

Read with: [README](../README.md) (the product), [AGENTS.md](../AGENTS.md) (rules for agents),
[development.md](development.md) (code layout, design decisions, releases),
[hardware-checks.md](hardware-checks.md) (every hand check and its result), [planned.md](planned.md)
(designs not built yet).

## 1. Getting started on another computer

### 1.1 Software

| What | Version used | Needed for |
|---|---|---|
| Windows 10 or 11, x64 | Windows 11 | everything: the app is Windows-only |
| Visual Studio 2026 (Community is enough) with **Desktop development with C++** and **C++ CMake tools for Windows** | MSVC 14.50, bundled CMake 4.2.3 | building; `CMakePresets.json` needs CMake ≥ 4.2 and the `Visual Studio 18 2026` generator |
| Git | any recent | cloning; keep the default `core.autocrlf=true` on Windows (see 1.6) |
| GitHub CLI `gh` | any recent | optional: issues, PRs, releases |
| Python 3 with Pillow | Python 3.13, Pillow 12.2 | only the design mockups (`pip install pillow`) |
| A Chromium browser (Edge or Chrome) | any | viewing and rendering the SVG mockups |
| An NVIDIA GPU with its driver | reference card: RTX 4070 | only hardware checks; see 1.5 |
| Windows optional feature **Graphics Tools** (`dxcap.exe`) | — | only the forced driver-reset checks |

No package manager and no download step: the three libraries are vendored in `third_party/`
(Dear ImGui 1.92.9b with its Win32 and DX11 backends, doctest, nlohmann/json 3.12.0), and the NVIDIA
libraries (`nvml.dll`, `nvapi64.dll`) are loaded at run time from System32, where the driver installs
them. No NVIDIA SDK is needed to build.

### 1.2 Build and test

`cmake` is not on `PATH` outside a Developer PowerShell; use the copy inside Visual Studio. With a
default installation of Visual Studio 2026 Community:

```powershell
git clone https://github.com/Rovey/gpu-auto-optimizer.git
cd gpu-auto-optimizer
$bin = "$env:ProgramFiles\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$bin\cmake.exe" --workflow --preset ci      # configure, build Release, run both test suites
```

A machine without the IDE can use **Build Tools for Visual Studio 2026** (`https://aka.ms/vs/18/stable/vs_buildtools.exe`,
workloads `Microsoft.VisualStudio.Workload.VCTools` with the CMake component); its CMake is in
`${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
The 0.4.10 release candidate was built that way on the test PC (CMake 4.3.1, MSVC 14.52) with the same result.

Expected: the build ends without warnings (everything is `/W4 /WX`), and CTest reports
`100% tests passed, 0 tests failed out of 2` (`core_tests` and `app_tests`). The executables are in
`build\Release\`: `gao.exe` (command line) and `GpuAutoOptimizer.exe` (window, tray, watchdog).

Other presets (see [development.md](development.md#build-and-test)):

```powershell
& "$bin\cmake.exe" --preset default; & "$bin\cmake.exe" --build --preset debug; & "$bin\ctest.exe" --preset debug
& "$bin\cmake.exe" --preset asan;    & "$bin\cmake.exe" --build --preset asan;  & "$bin\ctest.exe" --preset asan
```

The ASan tests need the MSVC `bin\Hostx64\x64` folder on `PATH` first, or the test binary fails to start
with `0xc0000135`; `.github/workflows/ci.yml` (job `asan`) shows how to find that folder with `vswhere`.

CI runs the `ci` workflow preset and the ASan job on `windows-2025` for every PR and every push to `main`.

### 1.3 Design mockups and the grid check

The approved UX design lives on branch `design/ux-redesign` (section 4). Its mockups are generated, not
drawn:

```powershell
git switch design/ux-redesign
python docs\design\mockups\generate_mockups.py $env:TEMP\mockups     # prints "ok"
python docs\design\mockups\gridcheck.py $env:TEMP\mockups             # ends with "PASS"
python docs\design\mockups\gridcheck.py                               # the committed SVGs: "PASS"
```

Expected, verified from a fresh clone on 2026-11-17:

- the four generated files equal the committed ones in `docs/design/mockups/` (`dashboard-after.svg`,
  `optimize-after.svg`, `result-not-kept-after.svg`, `run-recovering-after.svg`), byte for byte after
  line endings (see 1.6);
- `gridcheck.py` reports `UNEXPLAINED: 0` for each file (edges: dashboard 133, optimize 112, run 133,
  result 98) and ends with `PASS`.

The generator measures text with the Windows fonts **Segoe UI** (`C:\Windows\Fonts\segoeui.ttf`) and
**Segoe UI Semibold** (`seguisb.ttf`). The committed SVGs were made with these files (SHA-256):

- `segoeui.ttf`: `8134DBCD09E7B123C9A7F229D49CFFBCB01352CC72EA5E1076B65D0DCA9F73CD`
- `seguisb.ttf`: `2D9B22D71F72DE2823FEE5D9C8BC1B0FC32B2577C4C27B9EC6ABDBB8DF0E1731`

(`Get-FileHash C:\Windows\Fonts\segoeui.ttf`). With other font versions, text widths may differ by a
pixel and the byte comparison can fail; then `gridcheck.py` on the new output is the check that matters,
and the committed SVGs stay the reference. `gridcheck.py` needs only the Python standard library.

To look at a mockup, open the SVG in Edge or Chrome. To render one to PNG at its own size with whichever
of the two is installed:

```powershell
$browser = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe', 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty '(default)'
Start-Process -Wait $browser -ArgumentList '--headless', '--disable-gpu', '--window-size=1180,860', "--screenshot=$env:TEMP\dashboard.png", "file:///$((Resolve-Path docs\design\mockups\dashboard-after.svg) -replace '\\','/')"
```

### 1.4 Pictures of the real window

`GpuAutoOptimizer.exe --demo` plays a run in the window without touching the card or any file
(`--demo=run` skips the busy-card question). It refuses to start elevated. `tools/window-shot.ps1` starts
it, sizes the window and takes pictures of **that window only** with `PrintWindow`:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& .\tools\window-shot.ps1 -Exe .\build\Release\GpuAutoOptimizer.exe -Prefix .\out\shots\run -Mode '--demo=run' -At @(9, 28, 88)
```

Expected: one line `N s: PrintWindow WxH` per time and `closed`; the PNGs `out\shots\run-9.png` etc.
The window needs a signed-in desktop: started over SSH or from a scheduled task (session 0) the program
ends at once with exit code 1, and the script says so.
Never take pictures of the whole screen: other windows (mail, chats) end up in them. The UX plan's
stage 2 adds `--demo=<state>` and `tools/ui-shots.ps1` for every state at every size and scaling.

### 1.5 What needs an NVIDIA GPU

| Without a GPU (any Windows PC, CI) | With an NVIDIA GPU |
|---|---|
| building, `core_tests`, `app_tests`, ASan tests | `gao --status`, `--probe`, `--busy`, `--curve` (read only, not elevated) |
| the mockups and `gridcheck.py` | everything that writes to the card, elevated: runs (`--optimize`, `--undervolt`, the window's Optimize), `--apply`, `--reset`, `--set-core`, `--curve-flatten`, `--fan`, `--boot on` |
| `GpuAutoOptimizer.exe --demo` (the window starts without a card; the demo touches none) | `gao --stress <s>`, the tray watchdog, `tools/hwchecks.ps1` |

Hardware changes are checked by hand per [hardware-checks.md](hardware-checks.md) and recorded there;
AGENTS.md lists the targeted tests that take minutes instead of a 40-minute run.

### 1.6 Windows settings worth knowing

- **Elevation.** Commands that write to the GPU need an elevated shell; the window asks through UAC.
  The demo and the pictures must run *not* elevated.
- **Line endings.** With `core.autocrlf=true`, a checkout has CRLF in the SVGs while the generator writes
  LF. Compare with `git diff --no-index --ignore-cr-at-eol <a> <b>`, or clone with
  `git -c core.autocrlf=false clone ...` for a byte comparison.
- **Script policy.** `Set-ExecutionPolicy -Scope Process Bypass` before the scripts in `tools/`.
- **Local folders.** `build/`, `build-asan/` and `out/` are ignored; `out/` is the place for logs,
  pictures, test packages and extra worktrees.
- **The installed app.** A build that saves a tune while apply-at-logon is on replaces the copy in
  `C:\Program Files\GpuAutoOptimizer` (`The copy that starts at logon was updated`). After testing, put
  the release back with that release's own `gao --boot on`.

### 1.7 Working on real hardware: rules learned the hard way

- **Save `%ProgramData%\GpuAutoOptimizer\gao.json` first.** A run replaces the saved tune as soon as its
  overclock step ends.
- **A search can crash the PC.** The searches cross the card's edge on purpose. Usually that is a driver
  reset (a few seconds of black screen), but on GTX 10 cards a reset at a high clock has ended in a blue
  screen (DPC_WATCHDOG_VIOLATION), and an edge test once restarted the owner's PC. Say so before starting
  one on someone's machine, never chain risky tests unattended, and keep full runs for times when nobody
  needs the machine.
- **Other programs on the card skew the scores.** Even an idle editor that draws with the GPU cost 8 % in
  one series of tests. `gao --busy` must print `none`. The NVIDIA app's Instant Replay records while the
  app's window is open (about -2 %): run from the command line with the window closed.
- **Do not switch on "Automatic Tuning" in the NVIDIA app.** It starts its own scan that restarts itself
  and that this app cannot stop.
- **A killed tray app leaves the fans at their last manual speed.** Exit it from its tray menu; after a
  kill, `gao --fan auto` hands the fans back to the driver.
- **Do not `tail -F` a log that an elevated script still writes:** it held the file and made the script's
  `Add-Content` fail. Poll for a `DONE` marker instead (as `tools/hwchecks.ps1` writes).
- **Commit before applying a temporary test patch.** Undoing the patch with `git checkout -- <file>` also
  wipes uncommitted work in the same file.
- **Run the CLI from the classic console (`conhost`), not Windows Terminal,** for long runs on a weak
  card: Windows Terminal draws with the GPU and can stop updating while the run goes on (#91). Every run
  is also written to `%ProgramData%\GpuAutoOptimizer\run.log`.

## 2. Current project state (2026-11-22)

- **Latest release:** `v0.4.10`, published 2026-11-21: the annotated tag on the merge commit `7124045`
  "Release 0.4.10 (#95)". Version in `src/core/version.hpp`: `0.4.10`. The executables are unsigned; the
  release has the zip, its `.sha256` and a build provenance attestation.
- **Pre-releases of 0.4.10:** `v0.4.10-rc1`, `-rc2` and `-rc3` stay published with their tags.
- **Releases** are built by `.github/workflows/release.yml` from a `v*` tag that matches `version.hpp`,
  into a **draft** release; publishing is manual. A release branch with commits that the docs cite by hash
  is merged with a **merge commit**, as #95 was.
- **`main` after the release:** `git log --oneline v0.4.10..origin/main` lists what is not in a release yet.
- **`tools/window-shot.ps1`** (section 1.4) now says so when the program ends at once, as it does in a
  session without a desktop, instead of waiting for a window.

### 2.1 What v0.4.10 did not show on hardware

The checks are recorded in [hardware-checks.md](hardware-checks.md) (110 to 116 for this release).
Covered by unit tests and not seen on a real card:

- the "GPU could not be reached" advice, the skipped put-back after a stop on such a card, and the crash
  journal's `RECOVERY FAILED` and `GPU UNREACHABLE` verdicts: no GPU loss was provoked after these changes;
- the three check 112 fixes on a card that is hot at stock: no such card was available;
- the final code on the reference RTX 4070 (its six-profile run was with the code from before #93).

#84 stays open for its last criterion: tolerances backed by the scatter of a hot card, which needs a card
whose stock measurement completes hot (check 112 could not). Builds from before `97d9bf2` (`0.4.10-rc1`,
`-rc2`, `0.4.10-check112`) are not offered the final 0.4.10; their testers install it once by hand.

### 2.2 Open issues and pull requests

`gh issue list` and `gh pr list` are the current lists; this document does not repeat them.

### 2.3 Branches that stay on origin

| Branch | Why it stays | Next |
|---|---|---|
| `main` | The trunk | — |
| `design/ux-redesign` | The approved UX redesign, revision 5: proposal, mockups, generator, grid check, implementation plan (section 4). Documents only | Never merge as a whole; the implementation stages branch from `main` and take the spec from here |
| `test/check112` | The source of the private `0.4.10-check112` test build; `docs/hardware-checks.md` check 111 cites build `f2b235e`, which exists only on this branch | Keep while that citation needs to resolve |

Release and fix branches are deleted once their content is on `main`; the tags keep the release commits,
and the heads of merged PRs stay reachable on GitHub as `refs/pull/<n>/head`.

## 3. Where the history lives

All decisions are in the repository: product design decisions in
[development.md](development.md#design-decisions), hand checks and their numbers in
[hardware-checks.md](hardware-checks.md), designs not built yet in [planned.md](planned.md), the UX
redesign and its 15 decisions in `docs/design/ux-redesign-proposal.md` on `design/ux-redesign`, and the
reasoning of each change in its PR. Nothing in a local folder or conversation is needed to continue.
Deliberately not in the repository: test packages and messages for testers, raw logs from testers'
machines and from private test PCs (they contain personal details), and the build outputs in `out/`.

## 4. UX redesign: handover

### 4.1 Status

- **Revision 5 is approved** (2026-11-17), with every decision taken (D1–D15, see 4.3) and the
  implementation plan approved, including the estimates.
- **Nothing is built.** The branch holds documents only; `src/` on it equals `main`.
- **When to start:** the condition (v0.4.10 published as the Latest release) is met since 2026-11-21.
  The build itself starts when the owner says so.
- **How:** each stage is its own branch and PR from `main`, reviewed and merged on its own. Revision 5
  (the four mockups and sections 6–8 of the proposal) is the specification; a stage that cannot follow it
  stops and asks instead of deviating.

### 4.2 Files on `design/ux-redesign`

| File | What |
|---|---|
| `docs/design/ux-redesign-proposal.md` | The specification: audit (2–4), visual direction (5), design system (6: tokens, type, spacing and window sizes 6.3, status and phase rail 6.7, motion 6.10, layout grammar 6.11, summary 6.13), every screen (7), workflows (8), errors (9), accessibility (10), technical strategy (11: 11.1 window only, 11.2 the D7 engine reporting), risks (12), the implementation plan (13), decisions (14) |
| `docs/design/mockups/*-after.svg` | The four approved screens at 1180 × 860: Dashboard, Optimize/Choose, Run while recovering, Result not kept |
| `docs/design/mockups/generate_mockups.py` | Generates the four SVGs (deterministic; argument: output folder) |
| `docs/design/mockups/gridcheck.py` | Checks every element edge against the page grid and component offsets; `PASS`/`FAIL` |
| `docs/design/current/*.png` | Pictures of the current window, the "before" |

### 4.3 Decisions (section 14 of the proposal)

D1 body text 15 px · D2 no accent-tinted icons on tiles and cards · D3 profiles ordered by goal,
"Recommended" on All in one · D4 Undervolt in the same list · D5 during a run the curve open, the log
collapsed, no sparklines · D6 "not kept" in warning amber (status icon only) · **D7** the engine reporting
additions of 11.2 (phase, recovery steps, `ResultKind`, measurements of rejected results, crash ceiling) as
a separate, reporting-only PR (stage 1a); no behaviour change · D8 Undervolt suggested after Best/Max only
on a hot card where Undervolt can run (built in `b5f90f9` and `bcfcd65`, on `main`) · D9 the busy-card check shown on
Choose as information; the definitive check stays at Start · D10 **rejected** (no screen-recording caption)
· D11 dark theme only · D12 **rejected** (keyboard behaviour as today) · D13 start after 0.4.10 final ·
**D14** Best, Max and All in one also save `avg_temp_c` (counted-part average); the Dashboard compares
average temperature, the peak is secondary under "Show all"; older tunes stay valid and show no
temperature outcome (stage 1b, storage rules in 13.3) · **D15** a check mark before done phases in the
phase rail, in the done style, none on the active phase, no motion of its own (drawn in
`run-recovering-after.svg`).

Approved with conditions: **window sizes** (6.3: designed at 1180 × 860, fluid down to 1000 × 700 at
100/150/200 % scaling, reflow rules, never clipped or smaller text, no horizontal scroll of a normal page)
and **motion** (6.10: functional only, ease-out; hover/press 120 ms, disclosure/status/rail 180 ms,
recovery step 160 ms, loader 1.2 s per turn; no page transitions, bounce or pulsing; Abort and recovery
actions act at once; no extra load during measurements: at most 30 frames/s during a run; reduced motion
from the Windows "Animation effects" setting).

### 4.4 Implementation sequence

| # | Stage | Depends on | Size* | Risk* |
|---|---|---|---|---|
| 0 | Copy fixes (profile goal lines, Choose subtitle) with a test | — | S | very low |
| 1a | Engine reporting hooks (D7), reporting only | — | M | medium |
| 1b | Average temperature saved (D14), a data addition | — | S | low–medium |
| 2 | Tokens, layout, motion and primitives; `--demo=<state>`, `tools/ui-shots.ps1` | 0 | M–L | low |
| 3 | Shell and sidebar | 2 | S | low |
| 4 | Results | 1a, 2 | M–L | medium |
| 5 | Run and recovery (phase rail with D15, recovery timeline) | 1a, 2, 3 | L | medium |
| 6 | Optimize, Choose | 0, 2 | M | low |
| 7 | Dashboard (average temperature) | 1b, 2, 3 | M | low–medium |
| 8 | Fan, About and tray in the design system, functions unchanged | 2 | S–M | low |
| 9 | Consistency, regression, window overhead | 3–8 | S–M | low |

\* Estimates, not commitments: S ≤ 1 day, M 2–3 days, L 4–6 days. The files per stage are in 13.1 of the
proposal. 1a, 1b and 2 can run side by side; 1a and 1b touch the same three files in `src/app/`, so the
second to merge rebases on the first. After stage 2 every page keeps working, so any stage can be
released.

### 4.5 Test strategy (13.2 of the proposal)

Every stage: `cmake --workflow --preset ci` and the Debug build; existing tests unchanged (1a only adds;
1b edits the one assertion named in 13.3); `git diff main` outside `src/app/gui/`, `tests/`, `tools/`,
`CMakeLists.txt` and `docs/` empty (except 1a and 1b, limited to their additions); PrintWindow pictures of
every demo state at 1180 × 860 and 1000 × 700, each at 100, 150 and 200 %, compared with the mockups;
a keyboard pass. Per stage, the extra checks are listed in the 13.2 table (for example 1a: one real
driver reset compared with `run.log`; 1b: the downgrade to 0.4.10 loads a new `gao.json`; 2: contrast
≥ 4.5:1 and `test_motion.cpp`; 5: Abort in every phase and mid-transition). Stage 9 measures the
window's overhead on the stress score against the last release before the redesign (pass: no larger than
the old build's, within about 1 %).

## 5. Exact steps on a new computer

1. Install the software of 1.1.
2. `git clone https://github.com/Rovey/gpu-auto-optimizer.git` and `cd gpu-auto-optimizer`.
3. `& "$bin\cmake.exe" --workflow --preset ci` (1.2): expect 2 of 2 test suites passed.
4. `git fetch origin` and look at the branches of 2.3 (`git branch -r`).
5. For the UX design: `git switch design/ux-redesign`, run the commands of 1.3, expect `PASS`.
6. When the owner starts the redesign: UX stage 0 (and 1a, 1b, 2 in parallel) on new branches from `main`,
   per section 4.
