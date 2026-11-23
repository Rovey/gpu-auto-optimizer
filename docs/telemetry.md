# Telemetry: design and API contract

**Status: design (phase A). Nothing here is in the app yet, and nothing is sent.** The database migration is in
`supabase/migrations/`. No backend is deployed.

The goal is to learn from real runs on cards we do not have: which cards and drivers work, where runs fail, and what
they gain. Telemetry is off unless the user switches it on. It never decides anything about a tune.

## 1. What the code requires

The first plan assumed a few things the run code does not do. These choices follow from how it works.

| Topic | What the code does | Decision |
|---|---|---|
| A name | `gao::Telemetry` and `on_telemetry()` already mean the card's live sensors | The C++ code calls this a **run report**: `core/run_report.*`, `hw/report_io.*`, `app/common_report.cpp`. "Telemetry" stays the name of the backend |
| What counts as a started run | `ran == false` also covers a card that failed its five minutes at stock, and an Undervolt refused because the card was too hot at stock. Those are the runs we most need to see | A new `OptimizeHooks::started` callback marks the start, after the tuning lock and the busy-card question. Before it: a refusal, no event. After it: always one event, whatever `ran` says |
| All in one | It calls `run_optimize` and `run_undervolt` as steps | `started` fires once, in the top-level run (`hooks.busy_asked` is true in a step). The event is built by the caller, so a step never produces one |
| Where to integrate | The window (`worker.cpp`) and the CLI (`cli_runs.cpp`) each call the runs. Demo mode goes through the same worker | One shared function in the app layer, called from those two places. A demo run never calls it |
| Errors | `error` and `reason` are free text, and contain program names and file paths | **No text from a run is ever sent.** Error codes come from structured fields (section 5) |
| A result that was not kept | `UndervoltOutcome::not_kept` exists. The "slower than stock" rule of the clock search only leaves a sentence | Add `OptimizeResult::not_kept`, set where that rule fires. It changes no decision |
| The stock measurement of a refused run | `measure_stock` has the numbers, `fail()` drops them | Keep them in the outcome, so a card that is too hot at stock is reported with its temperature |
| The "before" of an undervolt on a saved tune | `result.baseline` is the **overclock** there, not stock | `on_saved_tune` is true and `stock_measurements` is null. Such a run is never compared with stock |
| Stop | There is one way to stop a run (`hooks.aborted`): the button, Ctrl+C and closing the window | The client sends `cancelled`. `aborted` stays in the enumeration, unused in schema version 1 |
| Card identity | NVML is not asked for PCI IDs, memory size or architecture yet | Read-only NVML calls, made at `started`, while the card is known to be healthy. The PCI bus ID is never stored or sent |
| Interrupted runs | A crash can happen between writing the event and clearing the marker | `event_id` is generated at the start and kept in the marker. A normal and an interrupted event of one run share it, so only one can ever count |

## 2. Consent and local files

All files are in the state folder, `%ProgramData%\GpuAutoOptimizer`, whose access rights do not change: administrators
write, users read. Runs are always elevated, so they can write there. A window without administrator rights shows the
state, and asks for the rights to change it, as the update-check switch does.

| File | Holds |
|---|---|
| `telemetry.json` | `enabled`, `consent_version`, `install_id`. A file of its own: `gao.json` is read elevated at every logon and stays free of this |
| `telemetry-pending.json` | The run in progress: `event_id`, `run_id`, start time, type, profile, card and driver |
| `telemetry-outbox.jsonl` | Events not delivered yet, each with its number of attempts |

- A missing, unreadable or older-`consent_version` `telemetry.json` means **off**. An update never switches it on.
- `install_id` is a random UUID, created when the user switches telemetry on. It is never derived from hardware.
- **Switching off** deletes the marker and the outbox and stops all sending. The `install_id` stays, so that the user
  can still ask for their records to be deleted.
- **Clear my data** sends the deletion request for the `install_id`. Only after the server confirmed it are the ID and
  local files removed. Offline, the ID is kept and the app says that the deletion is still to do.
- The word **anonymous** is not used, in the app or in the docs. A persistent random ID makes this data pseudonymous.

The switch in the window, on the About page, off by default:

> **Share diagnostic data about runs**
> After a run, sends your card and driver model, the app version, the profile, the result and its measurements, and
> error codes. Linked to a random ID, not to your name.
> **Never sent:** your name, the PC's name, the card's serial number or UUID, your files, program names, or logs.

## 3. Run lifecycle

```text
preflight refused ------------------------------> no event
      |
      v
started: telemetry on? read card, write the marker
      |
      v
the run (the report code takes no part in it)
      |
      v
build the event from the outcome -> append to the outbox -> delete the marker
      |
      v
send, outside the tuning lock
```

- **Next start:** a marker while `tuning_in_progress()` is false is a run that never finished. It becomes an
  `interrupted` event with the marker's IDs, and nothing that was not known is filled in.
- **Every failure is swallowed.** No report failure changes a run, its exit code or the state of the card. Report code
  that touches a driver runs inside `guarded_driver`.
- **Sending:** an elevated process, never during a run. The window sends in a background thread. The command line
  makes one short attempt after printing the result, at most 5 seconds, and leaves the rest in the outbox.
- **Outbox:** at most 50 events, none older than 30 days, at most 5 attempts per event, guarded by a mutex of its own
  because `gao.exe` and the tray app can both use it. A line that cannot be read is dropped.
- **HTTP:** the existing WinHTTP code, extended with POST. HTTPS only, certificate checks on, **no redirects**, 16 KB
  out, 1 KB in, and the body of an answer is used only to read `accepted`.

## 4. Event schema, version 1

One JSON object. A value that was not measured is `null`, never `0`. The `-1` of `Telemetry` and `StabilityResult`,
and a score of 0, become `null`. Unknown fields are refused by the server.

```json
{
  "schema_version": 1,
  "event_id": "6f1d3b52-8d7e-4c3a-9f42-1b6a0f3e7c11",
  "installation_id": "0c9e2a74-5b1f-4e8d-a3c6-7d2f91b4e650",
  "run_id": "b7a4c1e9-2f60-4d38-8e15-c94a6d0f2b73",
  "occurred_at": "2026-11-23T18:30:00Z",
  "application": { "version": "0.5.0", "channel": "stable" },
  "system": { "windows_version": "Windows 11", "windows_build": 26100 },
  "gpu": {
    "name": "NVIDIA GeForce RTX 4070",
    "architecture": "Ada",
    "vendor_id": 4318,
    "device_id": 10118,
    "subsystem_vendor_id": 4318,
    "subsystem_id": 5000,
    "vram_mb": 12282
  },
  "driver": { "version": "581.80" },
  "optimization": {
    "type": "all_in_one",
    "profile": "all_in_one",
    "on_saved_tune": false,
    "duration_ms": 2400000,
    "outcome": "success",
    "failed_stage": null,
    "error_code": null,
    "driver_resets": 0,
    "stock_restored": null,
    "tune_saved": true,
    "measurement_disturbed": false,
    "steps": { "overclock": "ok", "undervolt": "ok", "fan_tune": "ok", "final_test": "ok" }
  },
  "measurements": {
    "stock": {
      "verdict": "stable", "seconds": 300, "score": 5728, "power_w": 196, "core_mhz": 2789, "memory_mhz": 10495,
      "peak_temp_c": 67, "end_fan_pct": 73, "counted_seconds": null, "counted_score": null, "counted_power_w": null,
      "counted_core_mhz": null, "counted_memory_mhz": null, "counted_temp_c": null, "counted_fan_pct": null
    },
    "result": {
      "verdict": "stable", "seconds": 300, "score": 5820, "power_w": 180, "core_mhz": 2835, "memory_mhz": 12000,
      "peak_temp_c": 71, "end_fan_pct": 62, "counted_seconds": null, "counted_score": null, "counted_power_w": null,
      "counted_core_mhz": null, "counted_memory_mhz": null, "counted_temp_c": null, "counted_fan_pct": null
    }
  },
  "tune": {
    "core_offset_mhz": 135,
    "memory_offset_mhz": 1500,
    "power_limit_pct": 105,
    "undervolt_enabled": true,
    "undervolt_mv": 1035,
    "undervolt_mhz": 2850
  }
}
```

The values are an example. A measurement is one `StabilityResult`. The `counted_*` fields are the averages of its
last half, filled only for a run that covered its full length, and are what an undervolt was judged on. Compare two
runs on the same basis only.

| Field | Comes from |
|---|---|
| `measurements.stock` | `OptimizeOutcome::stock`, `UndervoltOutcome::before` (else the stock measurement that was kept), `AllInOneOutcome::stock`; null for a retest and on a saved tune |
| `measurements.result` | `result.soak`, `UndervoltOutcome::after` (else `result.after`), `AllInOneOutcome::now`, `RetestResult::test` |
| `driver_resets` | The existing `driver_resets()` of each outcome |
| `measurement_disturbed` | `busy_note` or `disturbed` not empty. Only the fact, never the text |
| `tune` | The saved `Profile`, when `tune_saved` |
| `steps` | All in one only: what became of its four steps |

## 5. Outcomes and error codes

The first row that fits decides the outcome.

| Outcome | When |
|---|---|
| `success` | A result is applied **and** saved (`ok`, or `ran && result.ok && saved`) |
| `cancelled` | `hooks.aborted()` is true at the end and no result was kept |
| `no_result` | The run finished but saved nothing: `not_kept`, or a result that could not be saved |
| `failed` | Every other run that started |
| `interrupted` | Built at the next start from a marker |

`error_code` is the first that fits:

| Code | When |
|---|---|
| `INTERNAL_ERROR` | The caller caught an exception |
| `GPU_UNREACHABLE` | The existing `gpu_unreachable()` |
| `STOCK_RESTORE_FAILED` | `stock_restored` is false |
| `NOT_KEPT` | `not_kept` |
| `TOO_HOT` | The deciding measurement ended `TooHot` |
| `DRIVER_RESET` | It ended `DeviceLost` |
| `STABILITY_CHECK_FAILED` | It ended `WrongResult` or `Stalled` |
| `MEASUREMENT_FAILED` | It ended `NoTelemetry` |
| `FAN_CONTROL_FAILED` | All in one stopped in its fan step |
| `UNCLASSIFIED` | None of these. Counted, so that this list can be improved |

`TOO_HOT`, `NOT_KEPT` and `UNCLASSIFIED` are additions to the first plan. A card that is too hot is the open question
of #84, and a code that claims a cause the app did not establish would make the data worse.

`failed_stage` is `stock`, `search`, `soak`, `undervolt`, `fan_tune`, `final_test` or `retest`, or null when it is not
known. `stock_restored` is null for a run that kept its result.

## 6. HTTP API

Public endpoints, with JWT verification off (`verify_jwt = false`). **The client is not trusted:** anything in an open
source binary can be extracted, so no key is embedded, and the data can be polluted by anyone who tries. The endpoint
URL is configuration. Requests carry the header `x-region: eu-central-1`. Supabase does not fail over to another region
then, so a failed request simply stays in the outbox. Deployment tests must read `x-sb-edge-region` in the response to
confirm that Frankfurt ran it.

### `POST /functions/v1/submit-telemetry`

Body: one event, at most 16 KB.

| Status | Body | Client does |
|---|---|---|
| 200 | `{"accepted":true,"duplicate":false}` | Removes the event |
| 200 | `{"accepted":true,"duplicate":true}` | Removes the event |
| 400 | `{"accepted":false,"error":"INVALID_EVENT"}` | Removes the event: it will not get better |
| 413 | `{"accepted":false,"error":"TOO_LARGE"}` | Removes the event |
| 429 | `{"accepted":false,"error":"RATE_LIMITED"}` | Keeps it, stops for this session |
| 5xx, or no answer | | Keeps it, counts an attempt, stops for this session |

Limits, without storing an IP address: 60 events per installation per 24 hours, and 1000 events per hour in total. The
second one caps cost and damage when someone rotates IDs. `occurred_at` must lie between 30 days ago and 5 minutes from
now. Errors never contain database text. Function logs never contain a request body.

### `POST /functions/v1/delete-telemetry`

Body: `{"installation_id":"<uuid>"}`. Deletes every record of that installation. The answer is
`{"deleted":true}` whether or not there were any, so the endpoint does not reveal which IDs exist. The random ID is the
only proof of ownership. It is readable by users of the same PC.

## 7. Database

`public.telemetry_runs`, one row per run. `id` is the `event_id`, `run_id` is unique, and constraints enforce the
enumerations, the ranges and the shape of the JSON. A run that was interrupted cannot claim a duration, a result or a
tune. Row level security is on with no policies, and `anon` and `authenticated` have no rights on the table or its
functions. Only the Edge Functions write, with the service role, from Supabase secrets.

Records are deleted after 12 months by a scheduled job (phase B).

## 8. Privacy boundaries

**Sent:** only the fields of section 4.

**Never sent or stored:** user or computer name, card UUID, serial number or PCI bus ID, program names, file paths, any
free text from a run, logs, IP addresses in application tables.

The hosting provider can see source IP addresses in its own network logs. Its retention must be reviewed before
production.

**Before production:** a published privacy policy naming the data controller, Supabase as processor (DPA and
subprocessors reviewed), the legal basis (consent is the intended one), withdrawal, retention, and how to ask for
deletion.

## 9. Validation

- **Normal CI:** unit tests with a fake transport. No live backend.
- **Backend:** valid, failed, duplicate and invalid events, rate limits, deletion, and direct anonymous access denied,
  against a development project.
- **Hardware:** on the test PC (GTX 1060). Crashes and interruptions are simulated, never provoked for this. A run on the
  reference RTX 4070 happens only on the owner's explicit word.

## 10. Open decisions for the owner

1. Who is named as data controller, with which public contact address.
2. Pay for Supabase Pro in production (about $25 a month). A Free project is paused after a week of low activity, and
   would lose incoming reports while paused.
3. Whether the hardware check of this feature stays on the test PC, or also runs on the RTX 4070.
