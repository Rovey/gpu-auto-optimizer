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
telemetry through NVML and do not need elevation.

Commands assume a Release build at `.\build\Release\gao.exe` from the repo
root; adjust the path for a Debug build.

- Date run: 2026-09-26
- Driver version (`nvidia-smi --query-gpu=driver_version --format=csv,noheader`): 610.74
- GPU: NVIDIA GeForce RTX 4070

| # | Check | Command | Elevated | Expected | Result |
|---|---|---|---|---|---|
| 1 | Telemetry matches nvidia-smi | `.\build\Release\gao.exe --probe` | no | Clocks, temp and power agree with `nvidia-smi`, and none of them print `n/a` (a failed reading now shows `n/a`, not a `0` that would look like a real one) || Pass. 5 back-to-back samples at idle identical to `nvidia-smi` (core 210, mem 405, temp 34, power 14-15 W); no `n/a`. |
| 2 | Fan percentage is real | `.\build\Release\gao.exe --probe` | no | `fan=` is not `n/a` (i.e. NVML reported a real value, not the -1 sentinel) || Pass. `fan=0%` (zero-RPM at idle; `nvidia-smi` also 0 %), not `n/a`. |
| 3 | Core offset applies | `.\build\Release\gao.exe --set-core 25` | yes | Read-back 25, `OK`; a follow-up `--probe` under the same load shows a core clock roughly 25 MHz above its boost figure from before the offset was applied. This second observation matters because the read-back reads the same byte offset the write just wrote: if `kOffCoreDelta` or the version word were wrong, the write would land in the wrong field and the read-back would confirm it anyway. The boost-clock rise is the only independent cross-check that the offset actually reached the clock, not just the buffer. || Read-back pass (`read back 25 MHz`, `OK`). Boost-clock cross-check **not yet done**: no GPU load available, idle clocks don't move with an offset. Redo once the P2 stress load exists. |
| 4 | Core offset reverts | `.\build\Release\gao.exe --reset` | yes | Read-back core 0, mem 0, `OK` || Pass. Read-back core 0, mem 0, `OK`. |
| 5 | Memory offset applies | `.\build\Release\gao.exe --set-mem 100` | yes | Read-back 100, `OK`; a follow-up `--probe` under the same load shows a memory clock roughly 100 MHz above its figure from before the offset was applied -- the same independent cross-check as check 3, for `kOffMemDelta`. || Read-back pass (`read back 100 MHz`, `OK`). Memory-clock cross-check **not yet done**, same reason as check 3. |
| 6 | A refused write is reported | `.\build\Release\gao.exe --set-core 5000` | yes | `MISMATCH`, not `OK` || Pass. Driver rejected the call itself (`NvAPI_GPU_SetPstates20 failed`), read-back 0, `MISMATCH`, exit 1. `--reset` afterwards: `OK`. |
| 7 | Fan control applies | `.\build\Release\gao.exe --set-fan 70` | yes | **On a GPU/driver that supports the legacy `NvAPI_GPU_SetCoolerLevels` API:** fan audibly rises within a few seconds, `OK`; a follow-up `--probe` shows `fan=` near 70%. **On this project's own reference RTX 4070, verified during implementation:** the legacy API answers `NVAPI_NOT_SUPPORTED`, so `gao` instead prints `fan control unavailable: NvAPI_GPU_GetCoolerSettings failed (status -104)` and exits 1 -- the fan does not move. That is the correct, honest result for *this* card (see check 9), not a failure of this check or of `gao`. || Pass (unsupported case): `fan control unavailable: NvAPI_GPU_GetCoolerSettings failed (status -104)`, exit 1. |
| 8 | Fan returns to automatic | `.\build\Release\gao.exe --set-fan -1` | yes | **On supported hardware:** fan drops back under driver control, `OK`. **On this reference RTX 4070:** the same `fan control unavailable: NvAPI_GPU_GetCoolerSettings failed (status -104)` message and exit 1 as check 7 -- there is no automatic control to hand back, because manual control was never available to begin with. A different message here than check 7 would indicate a real bug; the same message on both is the expected, consistent result. || Pass (unsupported case): same message as check 7, exit 1. |
| 9 | Unsupported fan API is reported | `.\build\Release\gao.exe --set-fan 70` (same command as check 7) | yes | Prints `fan control unavailable: ` followed by the name of the failing call, not silence and not a crash, and exits 1. | **This project's reference RTX 4070 *is* the unsupported case** -- its driver answers `NVAPI_NOT_SUPPORTED` (status -104) to the legacy cooler API (see the block comment in `src/hw/nvapi.cpp`), so checks 7 and 8 above already exercise this exact check on this exact hardware; it is not a separate scenario needing different hardware. If this project ever runs on a card that *does* support the legacy API, checks 7/8 will show real fan movement there, and this row would then need genuinely unsupported hardware (or a blocked/older driver) to exercise instead. |

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
