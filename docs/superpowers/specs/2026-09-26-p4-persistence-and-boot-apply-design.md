# P4 — Persistence and Boot-Apply

Date: 2026-09-26
Status: approved (design), not yet implemented
Parent: `2026-09-20-cpp-rewrite-design.md` §7 (this refines it). Builds on P3
(`2026-09-26-p3-search-and-journal-design.md`).

## 1. Goal

A tuning result survives a reboot. `--optimize` saves what it found; `gao --boot on` makes
Windows re-apply it at every logon; a result that keeps crashing the machine switches itself
off after three strikes; a driver update never gets an offset profile that was tested on an
older driver.

Success criteria:
- After `gao --optimize best` and `gao --boot on`, logging off and on re-applies the profile
  (read-back verified) with no visible window beyond a brief flash.
- Three logons that each crash within 2 minutes of applying stop the fourth from applying.
- A profile saved under another driver version is never applied.
- All decision logic is unit-tested in CI without a GPU.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Tray | **Moved to P5** | The tray belongs to the long-running GUI process (ImGui + message loop). A standalone tray process now would be rebuilt in P5, and "Open" has nothing to open before P5. User-approved. |
| Boot-time safety | Read-back + 3-strike + **driver version check**, no stress run | New drivers can change the V/F curve, making old offsets unstable. A stress run at every logon would cost 10 s of full load and fan noise. User-approved. |
| Scheduling | Task Scheduler task `GpuAutoOptimizer`, at logon, highest privileges, created with `schtasks.exe` | No service, no Run key (parent spec §7). `schtasks` avoids the COM Task Scheduler API. |
| Config | Minimal: the profile and the strike counter | Objectives follow from the preset; freeze ceilings live in the journal. Fewer fields, fewer ways to go stale. |

## 3. Commands

| Command | Does | Elevated |
|---|---|---|
| `gao --optimize <preset>` | as in P3, and on success saves the profile | yes |
| `gao --apply` | applies the saved profile (driver check, read-back; any failure → stock) | yes |
| `gao --boot on` | creates/replaces the logon task; resets the strike counter to 0 | yes |
| `gao --boot off` | deletes the logon task | yes |
| `gao --status` | saved profile, driver match, task present, strikes, last boot-log line | no |
| `gao --boot-apply` | what the task runs (hidden, see §4) | via task |

`--optimize` never enables boot-apply by itself.

## 4. `--boot-apply`

1. `FreeConsole()` immediately, so the logon shows at most a brief console flash.
2. Load `gao.json`. No profile → log "no saved profile", exit.
3. `boot_strikes >= 3` → log "disabled after 3 crashes; run `gao --boot on` to retry", exit.
4. Driver version differs from the profile's → log "driver changed (<saved> → <now>); run
   `gao --optimize` again", exit.
5. `boot_strikes += 1`, saved durably **before** touching the hardware.
6. `apply_profile`. Failure → stock, log the reason, exit (strike stays: a failed apply is a
   failed boot).
7. Log "applied …", sleep 2 minutes, set `boot_strikes = 0`, save, exit.

If the machine crashes within those 2 minutes, the strike stays. Three in a row and step 3
stops further attempts.

Every run appends one timestamped line to `%LOCALAPPDATA%\GpuAutoOptimizer\boot.log`: with no
visible window, that log is how the user (and `--status`) learns what happened.

`--apply` runs steps 2 (profile), 4 (driver) and 6 (apply), and prints instead of logging. It
does not touch the strike counter.

## 5. Data

`%LOCALAPPDATA%\GpuAutoOptimizer\gao.json`:

```json
{
  "profile": {
    "preset": "best", "power_pct": 105, "core_mhz": 135, "mem_mhz": 1050,
    "driver": "610.74", "saved_at": "2026-09-26 22:41"
  },
  "boot_strikes": 0
}
```

```cpp
struct Profile {
    Preset preset = Preset::BestOfMyGpu;
    int power_pct = 100, core_mhz = 0, mem_mhz = 0;
    std::string driver, saved_at;
};
struct Config {
    std::optional<Profile> profile;
    int boot_strikes = 0;
};
```

- `from_json` never throws: bad JSON or wrong types yield defaults (no profile, 0 strikes); a
  profile missing any field is treated as no profile.
- The config is written atomically and durably: temp file → `FlushFileBuffers` → `MoveFileExW`
  (`REPLACE_EXISTING | WRITE_THROUGH`). A crash mid-save leaves either the old or the new file,
  never half of one — the strike counter depends on it.
- The old `objectives` and `blacklisted_core_offsets` fields are dropped; an old file still
  loads (unknown fields are ignored).

## 6. Components

```
src/core/config.*     Profile, Config, to_json/from_json (rewritten)
src/core/boot.*       decide_boot(config, driver) -> BootDecision; apply_profile(gpu, profile)
src/hw/app_files.*    renamed from journal_file.*: app_dir(), journal/config/boot-log paths,
                      read_lines, append_line_durable, write_file_atomic, read_file
src/hw/nvml.*         + DriverVersion()
src/hw/boot_task.*    create / remove / exists via schtasks.exe (CreateProcess, exit code)
src/app/main.cpp      --apply, --boot on|off, --status, --boot-apply; --optimize saves
```

```cpp
enum class BootDecision { Apply, NoProfile, TooManyStrikes, DriverChanged };
BootDecision decide_boot(const Config& c, const std::string& driver);
// Sets power (when the profile asks for anything but 100 %, power control is
// required), core and mem, each verified by read-back via GpuControl. Any
// failure → reset_to_stock and false, with the reason in *why.
bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why);
```

Order in `decide_boot`: NoProfile, then TooManyStrikes, then DriverChanged, then Apply.

## 7. Error handling

- `--apply` / `--boot-apply` never leave a half-applied profile: any failed set → `reset_to_stock`.
- `schtasks` failure → the command prints its exit code and exits 1; `--status` reports the
  task state from `schtasks /Query`, not from memory.
- Config unreadable → treated as "no profile"; `--status` says the file could not be parsed.
- Config save failure in `--boot-apply` step 5 → do not apply (a strike that was not recorded
  would defeat 3-strike), log it.

## 8. Testing

Unit tests (CI):
- config round-trip with and without profile; bad JSON; wrong types; profile with a missing
  field → no profile; an old-format file (objectives, blacklist) loads without error.
- `decide_boot`: each of the four outcomes, and the order when several apply.
- `apply_profile`: success sets all three; a failing setter → reset, false, reason names the
  setter; no power control with power 100 → ok; with power 105 → false.

New rows in `docs/hardware-checks.md`:

| # | Check | Expected |
|---|---|---|
| 17 | `--optimize best`, then `--status` | profile shown, "driver matches" |
| 18 | `--reset`, then `--apply` | read-back equals the profile; `--probe` shows the limit |
| 19 | `--boot on`, log off and on | `boot.log` "applied"; after 2 min `--status` shows 0 strikes |
| 20 | set `boot_strikes` to 3 in `gao.json`, log off and on | not applied, log says why; `--boot on` resets strikes |
| 21 | change `driver` in `gao.json` | `--apply` refuses; boot-apply logs "driver changed" |
| 22 | `--boot off` | `schtasks /Query /TN GpuAutoOptimizer` finds nothing |

## 9. Out of scope

Tray and GUI (P5), release packaging (P6), multiple profiles, per-game profiles.
