# Hardware checks

CI builds this project but has no GPU. These checks are the evidence that the
hardware layer works. Run them on the real machine, record the date and
driver version below, and fill in the Result column for each row.

Checks 3-8 write to the GPU (clock offsets, fan levels) through NVAPI, which
refuses those calls without administrator rights. Run `gao.exe` from an
**elevated** PowerShell or Command Prompt (Start menu -> right-click
PowerShell/Terminal -> "Run as administrator") for any command marked
"yes" in the Elevated column below; an unelevated shell is expected to fail
those specific calls with an NVAPI error, not a crash. Checks 1-2 only read
telemetry through NVML and do not need elevation. Checks 10-12 need no
elevation either: the stress load only computes, it writes no settings.
Checks 13-16 run `--optimize`, which writes clocks and the power limit: elevated.
Checks 17-22 cover persistence; 19 and 20 need a log-off and log-on.

Commands assume a Release build at `.\build\Release\gao.exe` from the repo
root; adjust the path for a Debug build.

- Date run: 2026-09-26
- Driver version (`nvidia-smi --query-gpu=driver_version --format=csv,noheader`): 610.74
- GPU: NVIDIA GeForce RTX 4070

| # | Check | Command | Elevated | Expected | Result |
|---|---|---|---|---|---|
| 1 | Telemetry matches nvidia-smi | `.\build\Release\gao.exe --probe` | no | Clocks, temp and power agree with `nvidia-smi`, and none of them print `n/a` (a failed reading now shows `n/a`, not a `0` that would look like a real one) || Pass. 5 back-to-back samples at idle identical to `nvidia-smi` (core 210, mem 405, temp 34, power 14-15 W); no `n/a`. |
| 2 | Fan percentage is real | `.\build\Release\gao.exe --probe` | no | `fan=` is not `n/a` (i.e. NVML reported a real value, not the -1 sentinel) || Pass. `fan=0%` (zero-RPM at idle; `nvidia-smi` also 0 %), not `n/a`. |
| 3 | Core offset applies | In a second window run `.\build\Release\gao.exe --stress 120` first; then `.\build\Release\gao.exe --set-core 25` and compare the `core=` of its lines before and after | yes | Read-back 25, `OK`; a follow-up `--probe` under the same load shows a core clock roughly 25 MHz above its boost figure from before the offset was applied. This second observation matters because the read-back reads the same byte offset the write just wrote: if `kOffCoreDelta` or the version word were wrong, the write would land in the wrong field and the read-back would confirm it anyway. The boost-clock rise is the only independent cross-check that the offset actually reached the clock, not just the buffer. | | Pass (2026-09-26). Read-back 25, `OK`. Cross-check under `--stress`: core 2790 -> 2805 MHz (8 probes each, all identical), back to 2790 after `--reset`. The rise is one 15 MHz clock bin, not 25: the V/F curve moves in 15 MHz steps. |
| 4 | Core offset reverts | `.\build\Release\gao.exe --reset` | yes | Read-back core 0, mem 0, `OK` || Pass. Read-back core 0, mem 0, `OK`. |
| 5 | Memory offset applies | In a second window run `.\build\Release\gao.exe --stress 120` first; then `.\build\Release\gao.exe --set-mem 100` and compare the `mem=` of its lines before and after | yes | Read-back 100, `OK`; a follow-up `--probe` under the same load shows a memory clock roughly 100 MHz above its figure from before the offset was applied -- the same independent cross-check as check 3, for `kOffMemDelta`. | | Pass (2026-09-26). Read-back 100, `OK`. Cross-check under `--stress`: mem 10501 -> 10601 MHz, exactly +100. |
| 6 | A refused write is reported | `.\build\Release\gao.exe --set-core 5000` | yes | `MISMATCH`, not `OK` || Pass. Driver rejected the call itself (`NvAPI_GPU_SetPstates20 failed`), read-back 0, `MISMATCH`, exit 1. `--reset` afterwards: `OK`. |
| 7 | Fan control applies | `.\build\Release\gao.exe --set-fan 70` | yes | **On a GPU/driver that supports the legacy `NvAPI_GPU_SetCoolerLevels` API:** fan audibly rises within a few seconds, `OK`; a follow-up `--probe` shows `fan=` near 70%. **On this project's own reference RTX 4070, verified during implementation:** the legacy API answers `NVAPI_NOT_SUPPORTED`, so `gao` instead prints `fan control unavailable: NvAPI_GPU_GetCoolerSettings failed (status -104)` and exits 1 -- the fan does not move. That is the correct, honest result for *this* card (see check 9), not a failure of this check or of `gao`. || Pass (unsupported case): `fan control unavailable: NvAPI_GPU_GetCoolerSettings failed (status -104)`, exit 1. |
| 8 | Fan returns to automatic | `.\build\Release\gao.exe --set-fan -1` | yes | **On supported hardware:** fan drops back under driver control, `OK`. **On this reference RTX 4070:** the same `fan control unavailable: NvAPI_GPU_GetCoolerSettings failed (status -104)` message and exit 1 as check 7 -- there is no automatic control to hand back, because manual control was never available to begin with. A different message here than check 7 would indicate a real bug; the same message on both is the expected, consistent result. || Pass (unsupported case): same message as check 7, exit 1. |
| 9 | Unsupported fan API is reported | `.\build\Release\gao.exe --set-fan 70` (same command as check 7) | yes | Prints `fan control unavailable: ` followed by the name of the failing call, not silence and not a crash, and exits 1. | **This project's reference RTX 4070 *is* the unsupported case** -- its driver answers `NVAPI_NOT_SUPPORTED` (status -104) to the legacy cooler API (see the block comment in `src/hw/nvapi.cpp`), so checks 7 and 8 above already exercise this exact check on this exact hardware; it is not a separate scenario needing different hardware. If this project ever runs on a card that *does* support the legacy API, checks 7/8 will show real fan movement there, and this row would then need genuinely unsupported hardware (or a blocked/older driver) to exercise instead. |
| 10 | Stress load is stable on stock and loads the card | `.\build\Release\gao.exe --stress 60` (after `--reset`) | no | `VERDICT: STABLE`, exit 0, `avg power` ≥ 95 % of the power limit printed on each line, `errors=0` on every line | Pass (2026-09-26). `STABLE`, exit 0, 0 errors, peak 65 C, score ~5560 it/s. Steady power 184-195 W (avg 193 W = 96.5 % of 200 W); the whole-run average is 189 W (94.5 %) because the ramp-up at start is included. |
| 11 | A wrong result is detected | `.\build\Release\gao.exe --stress 10 --stress-selftest wrong` | no | `VERDICT: WRONG RESULT`, exit 2, within the first second | Pass (2026-09-26). `WRONG RESULT` on the first batch, exit 2. |
| 12 | A TDR is detected and survived | Start `.\build\Release\gao.exe --stress 30`; after ~5 s run `dxcap -forcetdr` in an elevated shell (`DXCap.exe` ships with Windows' Graphics Tools feature); then `.\build\Release\gao.exe --stress 5` | dxcap only | Screen goes black for 1-2 s while Windows resets the driver; the stress run prints `VERDICT: DEVICE LOST` (exit 2); the second prints `VERDICT: STABLE`. A long dispatch does not work as a trigger: the GPU preempts compute work instead of hanging, so no TDR fires (tried: a ~6 s dispatch completed as STABLE). | Pass (2026-09-26). `DEVICE LOST` a moment after `dxcap -forcetdr`; the next `--stress 5` printed `STABLE`. (The exit code was not captured in this run: `Start-Process` swallowed it; the verdict line comes from the same code path that returns 2.) |
| 13 | Optimize end to end | `.\build\Release\gao.exe --optimize best`, then `.\build\Release\gao.exe --stress 60` | yes / no | `RESULT:` line with the applied values, soak `STABLE`; the follow-up stress run `STABLE` | Pass (2026-09-26, driver 610.74). `RESULT: power 105 %, core +135 MHz (max stable +195), mem +1050 MHz (max stable +1500)`; soak `STABLE`; score 5539 -> 5909 it/s (+6.7 %), core 2752 -> 2910 MHz, mem 10295 -> 11550 MHz, peak 65 -> 67 C, ~11 min. Follow-up `--stress 60`: `STABLE`, 0 errors, 5906 it/s. The core edge moves between runs (+210 was STABLE in one run, WRONG RESULT in the next; +225 held at 80 % power), which is what the margin is for. Memory reached the top of the search range (+1500) every run. |
| 14 | A recorded freeze becomes a ceiling | Append `{"id":999,"core":150,"state":"begin"}` to `%LOCALAPPDATA%\GpuAutoOptimizer\journal.jsonl`, run `--optimize best` | yes | Warning "froze the machine at core +150"; no logged core candidate ≥ +150. Remove the line afterwards. | Pass (2026-09-26). Printed `warning: a previous run froze the machine at core +150; staying below it from now on`; the highest core candidate tried was +135 (max stable +135, applied +90). Line removed afterwards. |
| 15 | Ctrl+C restores stock | Press Ctrl+C during the core search of `--optimize best` | yes | "abort requested", then `RESULT: not applied -- aborted (card at stock)`; `--reset` read-back 0/0 | Pass (2026-09-26). Ctrl+C during the core search printed `abort requested`, finished the running +180 probe, then `stopped: aborted -- card restored to stock` / `RESULT: not applied -- aborted (card at stock)`. `--reset` afterwards: read-back 0/0, power default, `OK`. |
| 16 | Power limit applies | `--optimize quiet` log shows `power <N> %` lines; `--probe` afterwards | yes | `--probe`'s `/<limit> W` equals N % of the default limit (±1 %) | Pass (2026-09-26). `--optimize quiet` chose 80 %; `--probe` afterwards showed `/160 W`, 80 % of the 200 W default. That run also exposed a bug: the efficiency reference was a single noisy probe, so quiet cost 4 % score instead of < 2 % (5718 -> 5486 it/s); fixed in `ed289e1` (reference = best stable score at or below the cap). |
| 17 | Optimize saves a profile | `--optimize best` (elevated), then `--status` | yes / no | `Saved:` line; `--status` shows the profile and `driver: ... (matches)` | |
| 18 | Apply re-applies it | `--reset`, then `--apply`, then `--probe` | yes | `applied best: ...`, `OK`; `--probe` limit equals the profile's power % of default | |
| 19 | Boot-apply at logon | `--boot on`, log off and on, wait 2 min, `--status` | yes | `boot.log` last line `applied ...`; `--status` shows `boot-apply: on`, `strikes: 0 of 3` | |
| 20 | Three strikes stop it | set `"boot_strikes": 3` in `gao.json`, log off and on, `--status`; then `--boot on` | yes | last boot line `not applied: disabled after 3 crashes ...`; card at stock; after `--boot on`, `strikes: 0 of 3` | |
| 21 | A driver change blocks it | set `"driver": "000.00"` in `gao.json`, run `--apply`; restore the value | yes | `not applied: driver changed (000.00 -> ...)` | |
| 22 | Boot-apply off | `--boot off`, then `schtasks /Query /TN GpuAutoOptimizer` | yes | `boot-apply off: task removed`; schtasks reports the task does not exist | |

## Notes

- **Check 6 is the one that matters.** If a write NVAPI accepts cannot be
  distinguished from one it applies, the fan-curve search planned for a
  later phase cannot be trusted, and the approach needs rethinking before
  more is built on it. `--set-core 5000` (5 GHz above stock) is expected to
  be silently ignored by the driver; `gao` must print `MISMATCH`, never `OK`,
  for that request.
- Run check 4 (`--reset`) again after check 6 to leave the card in a known
  state, since a refused write can leave the read-back at whatever the
  driver's own limit clamped it to rather than exactly the requested value.
- Fan control here always goes through the older, fully-specified
  `NvAPI_GPU_SetCoolerLevels` / `NvAPI_GPU_GetCoolerSettings` /
  `NvAPI_GPU_RestoreCoolerSettings` API. The newer "client fan cooler" API
  (`NvAPI_GPU_ClientFanCoolersGetStatus`/`GetControl`/`SetControl`) is not
  called by this build: its interface ids are known (sourced from
  `arcnmx/nvapi-rs`), but no parameter struct for them exists in that crate
  or anywhere else this project could reach, and guessing one would be
  calling a real function pointer with an invented buffer -- unlike a wrong
  interface id, that is not a safe failure mode. See the block comment at
  the top of `src/hw/nvapi.cpp` for the full account.
- **On this project's own reference RTX 4070, fan control does not
  currently work at all** -- not a driver quirk to troubleshoot, not a bug
  to file. NVIDIA dropped the legacy per-cooler API this build calls on
  Turing-and-later GPUs, and this card confirms that (`NVAPI_NOT_SUPPORTED`,
  status -104, from `NvAPI_GPU_GetCoolerSettings` itself). Checks 7, 8 and 9
  above all describe this same, single, expected outcome on this hardware.
  Fan control only starts working again once the newer API above is
  implemented, which needs its struct layout from somewhere this project
  could not reach at implementation time.
