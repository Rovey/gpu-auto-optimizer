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

- Date run:
- Driver version (`nvidia-smi --query-gpu=driver_version --format=csv,noheader`):
- GPU:

| # | Check | Command | Elevated | Expected | Result |
|---|---|---|---|---|---|
| 1 | Telemetry matches nvidia-smi | `.\build\Release\gao.exe --probe` | no | Clocks, temp and power agree with `nvidia-smi` | |
| 2 | Fan percentage is real | `.\build\Release\gao.exe --probe` | no | `fan=` is not `n/a` (i.e. NVML reported a real value, not the -1 sentinel) | |
| 3 | Core offset applies | `.\build\Release\gao.exe --set-core 25` | yes | Read-back 25, `OK` | |
| 4 | Core offset reverts | `.\build\Release\gao.exe --reset` | yes | Read-back core 0, mem 0, `OK` | |
| 5 | Memory offset applies | `.\build\Release\gao.exe --set-mem 100` | yes | Read-back 100, `OK` | |
| 6 | A refused write is reported | `.\build\Release\gao.exe --set-core 5000` | yes | `MISMATCH`, not `OK` | |
| 7 | Fan control applies | `.\build\Release\gao.exe --set-fan 70` | yes | Fan audibly rises within a few seconds, `OK`; a follow-up `--probe` shows `fan=` near 70% | |
| 8 | Fan returns to automatic | `.\build\Release\gao.exe --set-fan -1` | yes | Fan drops back under driver control, `OK` | |
| 9 | Unsupported fan API is reported | Run `--set-fan 70` on a GPU/driver combination without cooler support | yes | Prints "fan control unavailable" and the failing call's name, not silence and not a crash | N/A on this machine's RTX 4070, which does answer to `NvAPI_GPU_SetCoolerLevels` -- see check 7/8. Revisit if this project is ever run on different hardware. |

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
