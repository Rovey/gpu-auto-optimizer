# Telemetry: design and API contract

**Status: backend implemented, reviewed and tested locally (phases B and C). Nothing here is in the app yet, nothing is sent, and no
backend is deployed.**

| Stage | State |
|---|---|
| Implemented | Migrations in `supabase/migrations/`, Edge Functions in `supabase/functions/` |
| Tested locally | Yes, on 2026-11-23: see section 9 |
| Tested against a real Supabase development project | **No**: the procedure is in `docs/telemetry-dev-verification.md` |
| Verified in Frankfurt | **No** |
| Approved for production | **No**: see section 12 |

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
| Stop | There is one way to stop a run (`hooks.aborted`): the button, Ctrl+C and closing the window | The client sends `cancelled`. `aborted` is refused as a run outcome in schema version 1. It stays a **measurement** verdict, because `Verdict::Aborted` is a real `StabilityResult` value |
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
    "additional_error_codes": [],
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

What the event builder must do, because the server refuses anything else:

- **Every key is present.** A value that is not known is an explicit `null`. A missing key and an unknown key are both
  refused.
- **A measurement that never ran is `null`.** `StabilityResult` starts as `Stable` with `seconds` 0, so sending it would
  claim a stable run. A `stable` measurement needs `seconds` above 0.
- **`counted_*` are all `null`** unless `counted.seconds` is above 0, which only a `stable` measurement has.
- **Whole numbers** stay whole: watts, MHz, temperatures, fan percentages and the tune. `undervolt_mv` is `volt_uv / 1000`
  and `undervolt_mhz` is `freq_khz / 1000`. Without an undervolt both are `null`.
- **A reading outside its range is sent as `null`** (the ranges are in the migration). Otherwise one bad sensor loses the
  whole event.
- `gpu.architecture` is `Pascal`, `Volta`, `Turing`, `Ampere`, `Ada` or `Blackwell`, otherwise `null`. `gpu.name` starts with
  `NVIDIA`, `GeForce`, `Quadro`, `TITAN` or `Tesla`, then a space and at most 56 letters, digits, spaces and `._+()-`,
  otherwise `null`. A name of any other form could hold free text.
- `system.windows_version` is `Windows 10`, `Windows 11` or `Windows Server 20xx`, otherwise `null`.
- `application.version` is `x.y.z`, optionally with `-rc`, `-check`, `-test` or `-dev` and a number.
  `application.channel` is `prerelease` exactly when it has that suffix (`0.5.0-rc1`).
- Every text value is printable ASCII.
- `event_id`, `run_id` and `installation_id` are random version 4 UUIDs in lower case. `occurred_at` is UTC in the form
  `2026-11-23T18:30:00Z`.

| Field | Comes from |
|---|---|
| `measurements.stock` | `OptimizeOutcome::stock`, `UndervoltOutcome::before` (else the stock measurement that was kept), `AllInOneOutcome::stock`; null for a retest and on a saved tune |
| `measurements.result` | `result.soak`, `UndervoltOutcome::after` (else `result.after`), `AllInOneOutcome::now`, `RetestResult::test` |
| `driver_resets` | The existing `driver_resets()` of each outcome |
| `measurement_disturbed` | `busy_note` or `disturbed` not empty. Only the fact, never the text |
| `error_code`, `additional_error_codes` | Section 5 |
| `tune` | The saved `Profile`, when `tune_saved` |
| `steps` | All in one only: what became of its four steps |

## 5. Outcomes and error codes

The first row that fits decides the outcome.

| Outcome | When |
|---|---|
| `success` | A result is applied **and** saved (`ok`, or `ran && result.ok && saved`) |
| `cancelled` | `hooks.aborted()` is true at the end. A retest can end this way with a lower step saved |
| `no_result` | The run finished but saved nothing: `not_kept`, or a result that could not be saved |
| `failed` | Every other run that started |
| `interrupted` | Built at the next start from a marker |

A run can fail in several ways at once: a search that throws and then cannot reset the card, or a retest that fails and
cannot save its lower step. One code cannot hold that, so an event has two fields:

- **`error_code`** is the first code in the table below that applies to the run.
- **`additional_error_codes`** holds every other code that applies, in the order of the table. It is `[]` when there are
  none, and it is always present. The list is closed: no other value and no free text is accepted.

One set of causes therefore has exactly one spelling, and the server refuses any other.

| Order | Code | When |
|---|---|---|
| 1 | `GPU_UNREACHABLE` | The existing `gpu_unreachable()`: the reset to stock failed after a driver reset |
| 2 | `STOCK_RESTORE_FAILED` | `stock_restored` is false, without a driver reset |
| 3 | `INTERNAL_ERROR` | The caller caught an exception, or the search ended in an access violation |
| 4 | `TOO_HOT` | The deciding measurement ended `TooHot` |
| 5 | `DRIVER_RESET` | It ended `DeviceLost` |
| 6 | `STABILITY_CHECK_FAILED` | It ended `WrongResult` or `Stalled` |
| 7 | `MEASUREMENT_FAILED` | It ended `NoTelemetry` |
| 8 | `FAN_CONTROL_FAILED` | All in one stopped in its fan step |
| 9 | `UNCLASSIFIED` | No known cause. Counted, so that this list can be improved |
| 10 | `NOT_KEPT` | `not_kept` |
| 11 | `SAVE_FAILED` | A write of the saved tune failed: the result or a lower step could not be saved, or All in one could not put the tune from before the run back |

The state of the card comes first because it is what a user must act on: **a failed reset to stock is always `error_code`
and can never be hidden behind `INTERNAL_ERROR`.** The cause of the run stays visible next to it. `TOO_HOT`, `NOT_KEPT`,
`SAVE_FAILED` and `UNCLASSIFIED` are additions to the first plan. A card that is too hot is the open question of #84, and
a code that claims a cause the app did not establish would make the data worse.

`failed_stage` is `stock`, `search`, `soak`, `undervolt`, `fan_tune`, `final_test` or `retest`, or null when it is not
known. `stock_restored` is null for a run that kept its result.

The database enforces what each outcome may say:

| Outcome | Codes it can have | Also |
|---|---|---|
| `success` | None | `tune_saved` true, a tune, a `stable` result, no stage, `stock_restored` null |
| `cancelled` | None, or any of `GPU_UNREACHABLE`, `STOCK_RESTORE_FAILED`, `INTERNAL_ERROR`, `SAVE_FAILED` | No stage: a stop blames nothing |
| `no_result` | `NOT_KEPT` or `SAVE_FAILED`, optionally with `GPU_UNREACHABLE` or `STOCK_RESTORE_FAILED` | `NOT_KEPT` is a reason, not a technical failure |
| `failed` | A first code other than `NOT_KEPT` and `SAVE_FAILED`. `SAVE_FAILED` can follow it, `NOT_KEPT` cannot | |
| `interrupted` | None | Only what the marker holds. No duration, resets, measurements, tune, steps, stage or stock state |

- `tune_saved` is true only on `success`, or on a `failed` or `cancelled` **retest** that saved a lower step.
- `SAVE_FAILED` needs `tune_saved` false.
- `stock_restored` false needs `GPU_UNREACHABLE` or `STOCK_RESTORE_FAILED` as `error_code`, and those two need
  `stock_restored` false. They exclude each other and never appear in `additional_error_codes`.
- `stock_restored` null means **not known**, which is what a run that threw must send today (see below).

**Known application limitation (follow-up F1 in `docs/planned.md`).** Following an exception, the result of the reset to
stock is currently discarded, and `UndervoltResult::stock_restored` defaults to `true`, so such a run may incorrectly read
as "restored". Until the application code is corrected, telemetry **must send `null`** for the uncertain restoration status
of any run that threw. An unverified restoration is never reported as successful.

- `UNCLASSIFIED` can follow only a failed reset to stock, and be followed only by `SAVE_FAILED`.
- `FAN_CONTROL_FAILED` is All in one only, with the stage `fan_tune` or `final_test`.
- A run that is not `interrupted` always has `duration_ms`, `driver_resets` and `tune_saved`.

**What the client must add in the next phase.** The run code holds some of these facts only as text, and no text may be
sent. Without these changes the events stay valid but say less:

| Fact | Today | Needed |
|---|---|---|
| The reset after an exception | `run_optimize` and `run_undervolt` call `reset_to_stock()` in their `catch` and drop the answer. The default of `stock_restored` then differs: `false` in `OptimizeResult`, `true` in `UndervoltResult` | Keep the answer. Until then send `null`. Never read `stock_restored` from an outcome that threw: the `true` would hide a failed reset |
| An access violation in the search | Only a sentence in `reason` | A flag on the outcome, or it reports as `UNCLASSIFIED` |
| How All in one and a retest left the card | Only the end of `error` (`stock_end`). A step's own `stock_restored` can be false while the retry after it succeeded | A field with the final state |
| All in one could not put the old tune back | Only a sentence appended to `error` | A flag, reported as `SAVE_FAILED` |

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

A request that is not `POST` gets 405. A body that is not JSON, or has another content type, gets 400. There are no CORS
headers: a browser is not a client. A 500 is `{"accepted":false,"error":"UNAVAILABLE"}`.

Limits, enforced in the database, without storing an IP address:

- **60 events per installation per UTC calendar day.**
- **1,000 events per UTC hour globally**, over all installations.

These are **fixed UTC windows, not rolling 24-hour windows**: a counter starts at zero at 00:00 UTC, or at the start of the
UTC hour. An installation may therefore send **up to 120 events across a UTC midnight boundary**, 60 just before it and 60
just after.

A duplicate takes no hit, also when it arrives at the same moment as the original, so a client can always clear its
outbox. An event the database refuses does take one. `occurred_at` must lie between 30 days ago and 5 minutes from now.
Errors never contain database text. Function logs never contain a request body or an ID.

### `POST /functions/v1/delete-telemetry`

Body: exactly `{"installation_id":"<uuid>"}`. Deletes every record of that installation.

| Status | Body | Client does |
|---|---|---|
| 200 | `{"deleted":true}`, whether or not there were records | Removes the ID and the local files |
| 400 | `{"deleted":false,"error":"INVALID_REQUEST"}` | Keeps the ID, tells the user |
| 413 | `{"deleted":false,"error":"TOO_LARGE"}` | Keeps the ID, tells the user |
| 429 | `{"deleted":false,"error":"RATE_LIMITED"}` | Keeps the ID, tries again later |
| 5xx, or no answer | | Keeps the ID, tries again later |

Limits: 10 requests per installation per UTC calendar day, and 600 per UTC hour globally, in the same fixed UTC windows. They count whether or not the ID exists.

**Ownership.** The random ID is the only proof. Whoever holds it can delete that installation's records or submit in its
name. It cannot read anything, because no endpoint returns data. It is readable by other users of the same PC
(`%ProgramData%`). The worst case is the loss or pollution of one PC's diagnostics, which is not worth accounts or a
second secret, so the design stays as it is. Keep the ID out of logs and support requests.

## 7. Database

`public.telemetry_runs`, one row per run. `id` is the `event_id`, `run_id` is unique, and constraints enforce the
enumerations, the ranges, the shape of the JSON and the rules of section 5.

**Access.** Row level security is on for both tables, with no policies, and no role but the owner has any right on them.
`anon` and `authenticated` can execute nothing. The service role can execute `telemetry_submit` and `telemetry_delete`
and nothing else, so a leaked service key can neither read the data, nor write past the limits and the constraints, nor run
the purge. Those two functions are `SECURITY DEFINER` for that reason: they take typed arguments, run no dynamic SQL, name
every object with its schema and search `pg_catalog, pg_temp`. An empty `search_path` is not enough: PostgreSQL then searches
`pg_temp` first for types, and a caller's temporary domain named `uuid` or `text` would run its check as the owner.
`supabase/tests/audit.sql` checks all of this in the catalog.

**Rate-limit counters.** `public.telemetry_rate_limits` holds a counter per bucket and time window. An installation appears
there as the SHA-256 of its ID, also after its records were deleted, until the counter is removed.

The table cannot grow without bound when clients rotate installation IDs:

- A new counter is only created for a request the global limit let in. Once the hour is full, a new ID writes nothing.
- That bounds a UTC day to 38,400 installation counters (24 hours of 1000 submissions and 600 deletions).
- Every admitted request removes up to 10 counters of ended windows and adds at most one, so the table drains by itself,
  also if the scheduled job stops. Together with the job, at most two days of counters exist: 76,800 rows, about 19 MB
  with their indexes.
- A counter is only removed after its window has ended. Windows last at most a UTC day and are aligned to it, so a counter
  of an earlier day is over and an active one cannot be removed, which would reset a limit.

**Retention.** `telemetry_purge_expired()` deletes runs received more than 12 months ago and all counters of ended windows.
It takes no argument, so no caller can move the boundary. Only its owner can run it: the `pg_cron` job
`telemetry-retention`, daily at 03:17 UTC. Provider backups are outside this job; how long they keep deleted rows must be
checked (section 12).

## 8. Privacy boundaries

The data is **pseudonymous, not anonymous**.

**Sent:** only the fields of section 4.

**Never sent or stored:** user or computer name, card UUID, serial number or PCI bus ID, program names, file paths, any
free text from a run, exception text, logs, IP addresses in application tables.

**IP addresses are processed by the provider.** The application does not read or store them, but a request reaches
Supabase's network. Its documentation lists the client IP (`cf_connecting_ip`) in API gateway logs, kept 1 day on Free and
7 days on Pro. Whether Edge Function invocation logs hold it was not established. The privacy policy must not say that no
IP address is processed.

## 9. Validation

Run on 2026-11-23 from a clean checkout, on Windows and on Linux (both backend commands, in containers on the Docker engine of the same PC). No GPU took part: all events are synthetic. **All of this is local.** No hosted Supabase
project was used, and nothing here says anything about Frankfurt.

| Check | Command | Result |
|---|---|---|
| Repository CI | `cmake --workflow --preset ci` | 2 of 2 test programs: 610 test cases, 12424 assertions passed, 0 warnings |
| Schema, endpoints, limits, error sets, catalog audit | `deno test --no-lock --allow-read --allow-env --allow-sys supabase/tests/`, on PostgreSQL 17 in process (PGlite). No Docker, and no network once the npm package is cached | 274 passed |
| The real local stack: migrations from empty volumes, catalog audit, role tokens refused by the database (`42501`) with `service_role` as control, concurrency, cleanup under load, the scheduler running the purge | `node supabase/tests/stack/run.mjs` | 104 passed |

The concurrency part of the stack check: 90 events of one installation gave 60 rows, 60 hits and 30 answers of 429, with
the global counter also at exactly 60. One event sent 10 times at once gave 1 row and 1 hit. The last 10 global hits, raced
by 30 installations, went to exactly 10. Rotating IDs on a full hour added no counter.

**In GitHub Actions**, the `backend` job of `.github/workflows/ci.yml` runs both backend commands. It needs no secret and no
Supabase project, and it has no path filter or `continue-on-error`, so a failure fails the workflow. A test marked `ignore`
or `only` fails it too. **That job has not run on GitHub yet**, because this branch has not been pushed. Its Linux commands were run by hand as described above, not on a GitHub runner.

**Not tested:** a hosted Supabase project, its gateway and secret keys, the region header, and the Windows client, which
does not exist yet.

**Hardware:** the client check belongs on the test PC (GTX 1060). Crashes and interruptions are simulated, never provoked.
The RTX 4070 takes no part.

## 10. Open decisions for the owner

1. Who is named as data controller, with which public contact address.
2. Pay for Supabase Pro in production (about $25 a month). A Free project is paused after a week of low activity, and
   would lose incoming reports while paused.
3. The global limit. A row measured 2.9 KB with its indexes, so someone sending 1000 valid events an hour adds about
   70 MB a day: the 500 MB of Free in about a week. A lower limit reduces this, and also the number of real events that
   can arrive in a busy hour.

## 11. Backend setup

**Secrets.** Nothing is set by hand. Supabase injects `SUPABASE_URL` and `SUPABASE_SECRET_KEYS`, and the function uses the
key named `default`, falling back to the legacy `SUPABASE_SERVICE_ROLE_KEY`. A service key never goes into the Windows
application, the repository or a response.

**Running the tests from a clean checkout:**

```powershell
deno test --no-lock --allow-read --allow-env --allow-sys supabase/tests/   # Deno 2.9.6
node supabase/tests/stack/run.mjs                                          # Node 20 or later, Docker running
```

The stack check starts its own stack from empty volumes and removes it again, which takes a few minutes. It refuses a
checkout linked to a hosted project and any API that is not local, and it fails if an image differs from this list, which
is what Supabase CLI 3.3.0 starts:

| Service | Image |
|---|---|
| Database (with `pg_cron`) | `public.ecr.aws/supabase/postgres:17.12.0.002` |
| PostgREST | `public.ecr.aws/supabase/postgrest:v16.6` |
| Gateway | `public.ecr.aws/supabase/kong:2.8.1` |
| Edge Functions | `public.ecr.aws/supabase/edge-runtime:v1.79.3` |

**Local development** needs Docker:

```powershell
npx supabase@3.3.0 start -x gotrue,realtime,storage-api,imgproxy,mailpit,studio,postgres-meta,logflare,vector,supavisor
# functions: http://127.0.0.1:54321/functions/v1/submit-telemetry
npx supabase@3.3.0 stop --no-backup
```

**Deploying to a development project** and verifying the database and function regions separately:
`docs/telemetry-dev-verification.md`. None of it has been run.

**Monitoring retention and size:**

```sql
select d.status, d.start_time, d.return_message
from cron.job_run_details d join cron.job j using (jobid)
where j.jobname = 'telemetry-retention' order by d.start_time desc limit 7;

select count(*) from public.telemetry_runs where received_at < now() - interval '12 months 2 days';  -- must be 0
select pg_size_pretty(pg_total_relation_size('public.telemetry_runs'));
```

**Known limitations**

- Anyone can submit valid-looking events: the client is open source. The limits bound the damage, they do not prevent it.
- A flood can use up the global limit. Real events then get 429 and wait in the outbox.
- The limits are fixed UTC windows, not rolling ones: an installation may send up to 120 events across a UTC midnight
  boundary.
- A request refused with 429 still counts as a function invocation. The limits protect the database, not the provider's
  invocation and egress quotas.
- A `duplicate` answer confirms that an `event_id` or `run_id` exists to someone who already holds that random UUID.
- A deletion takes slightly longer when there are rows. This was not measured, and using it needs a guessed random UUID.
- An event sent after a deletion creates a new record, so the client clears its outbox first.

## 12. Privacy and operational readiness

**This is a checklist, not a statement of GDPR compliance.** Nothing here is legal advice, and no item is approved.

- **Provider docs:** read in Supabase's public documentation on 2026-11-23. Not measured on a project of ours.
- **Owner:** a decision only the owner can take.
- **Confirm:** still to be confirmed with the provider or measured on a development project.

| Item | What is known | Status |
|---|---|---|
| Data controller and public contact | Not decided. No details are to be invented | **Owner** |
| Privacy policy and consent wording | A draft of the switch is in section 2. No policy exists. It must name the controller, Supabase as processor, the legal basis (consent is the intended one), withdrawal, retention, deletion, and what the provider's logs hold | **Owner** |
| Supabase DPA | A DPA is published. It allows processing wherever Supabase or its subprocessors have facilities, so a Frankfurt region alone does not keep all processing in the EU | Provider docs. **Owner** reviews it |
| Subprocessors | The list of 2026-10-12 includes AWS, Google, Cloudflare, Vercel, Sentry, OpenAI and HubSpot, mostly in the US. Which of them touch this data is not established | Provider docs. **Confirm** |
| API gateway IP logging | The client IP (`cf_connecting_ip`) is in API gateway logs. API and database logs are kept 1 day on Free, 7 on Pro, 28 on Team, 90 on Enterprise | Provider docs |
| Edge Function logs | Whether invocation logs hold the client IP is not stated | **Confirm** |
| Database backups | Daily backups on Pro (7 days), Team (14) and Enterprise (up to 30). None listed for Free. How long a deleted row stays in a backup is not stated. A backup taken before a deletion presumably holds the row until it expires | Provider docs. **Confirm** |
| Retention job | Implemented, and ran on the local stack. Its real execution on a hosted project is not shown | **Confirm** on a development project |
| Deletion | Tested locally. Backups and provider logs are outside it | **Confirm** on a development project |
| Storage and growth | A row is about 2.9 KB with indexes: 12,000 runs a year is about 35 MB. The ceiling at the global limit is about 70 MB a day, so 500 MB in about a week | Measured locally |
| Free or Pro | Free: 500 MB per project, 500,000 function invocations a month, 5 GB egress per organization, paused after a week of low activity, no daily backups. Pro is about $25 a month, and usage above its quota is charged | Provider docs. **Owner** |
| Cost under a flood | Refused requests still count as invocations and egress. Whether a spend cap is on must be checked before any paid plan is used | **Owner**, **Confirm** |
| Database and function region | Neither verified | **Confirm**: `docs/telemetry-dev-verification.md` |
| Windows client | Not built. Telemetry stays off until later phases | Open |

Sources: [Data Processing Addendum](https://supabase.com/legal/customer-resources/data-processing-addendum),
[subprocessors](https://supabase.com/legal/subprocessor-list/October-12-2026.pdf),
[logs](https://supabase.com/docs/guides/telemetry/logs),
[API errors in logs](https://supabase.com/docs/guides/troubleshooting/discovering-and-interpreting-api-errors-in-the-logs-7xREI9),
[backups](https://supabase.com/docs/guides/platform/backups),
[regional invocations](https://supabase.com/docs/guides/functions/regional-invocation),
[billing](https://supabase.com/docs/guides/platform/billing-on-supabase),
[pricing](https://supabase.com/pricing),
[project pausing](https://supabase.com/kb/faqs/platform/free-project-pausing).
