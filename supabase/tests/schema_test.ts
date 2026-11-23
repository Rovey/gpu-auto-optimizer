// The database on its own: constraints, validators, grants. The cases of the
// first review (phase A) come first; they are kept, with a whole `steps` object
// where the validator now requires one.
import { toRow } from "../functions/_shared/telemetry.ts";
import { asRole, assert, assertEq, auditSql, docExample, freshDb, newId, type Row } from "./helpers.ts";

const db = await freshDb();
const example = toRow(docExample());
const jsonCols = new Set(["stock_measurements", "result_measurements", "tune", "steps"]);

// As the owner, which is who telemetry_submit runs as.
async function insert(r: Row, role = "postgres") {
  const keys = Object.keys(r);
  const vals = keys.map((k) => (jsonCols.has(k) && r[k] !== null ? JSON.stringify(r[k]) : r[k]));
  const ph = keys.map((k, i) => `$${i + 1}${jsonCols.has(k) ? "::jsonb" : ""}`);
  await asRole(db, role, () => db.query(`insert into public.telemetry_runs (${keys.join(",")}) values (${ph.join(",")})`, vals));
}
const fresh = (over: Row = {}): Row => ({ ...example, id: newId(), run_id: newId(), ...over });
const accepted = (name: string, r: () => Row) => Deno.test(`accepted: ${name}`, async () => await insert(r()));
const refused = (name: string, r: () => Row, needle?: string) =>
  Deno.test(`refused: ${name}`, async () => {
    let error = "";
    try {
      await insert(r());
    } catch (e) {
      error = (e as Error).message;
    }
    assert(error, "the insert went through");
    if (needle) assert(error.includes(needle), `expected "${needle}" in: ${error}`);
  });

const failed: Row = {
  outcome: "failed", tune_saved: false, tune: null, steps: null, optimization_type: "undervolt", profile: "undervolt",
  failed_stage: "stock", error_code: "TOO_HOT", stock_restored: true, result_measurements: null,
};
const noResult: Row = { ...failed, outcome: "no_result", error_code: "NOT_KEPT", failed_stage: null };
const cancelled: Row = { ...failed, outcome: "cancelled", error_code: null, failed_stage: null };
const interrupted: Row = {
  outcome: "interrupted", duration_ms: null, driver_resets: null, stock_restored: null, tune_saved: null,
  result_measurements: null, stock_measurements: null, tune: null, steps: null,
};
const retest: Row = {
  optimization_type: "saved_tune_retest", profile: "all_in_one", stock_measurements: null, steps: null,
};
const allSteps = { overclock: "ok", undervolt: "ok", fan_tune: "ok", final_test: "ok" };
const tune = example.tune as Row;

// ---- the phase A cases
accepted("the example in the doc", () => ({ ...example }));
accepted("failed run", () => fresh(failed));
accepted("no_result, not kept", () => fresh(noResult));
accepted("cancelled without a code", () => fresh(cancelled));
accepted("interrupted", () => fresh(interrupted));
accepted("card fields unknown", () => fresh({ ...interrupted, gpu_name: null, device_id: null, vram_mb: null, driver_version: null }));
accepted("missing readings are null", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, score: null, power_w: null } }));
accepted("prerelease version", () => fresh({ app_version: "0.5.0-rc1", app_channel: "prerelease" }));

refused("duplicate event", () => ({ ...example }), "duplicate key");
refused("same run, other event", () => ({ ...example, id: newId() }), "duplicate key");
refused("bad outcome", () => fresh({ outcome: "great" }));
refused("bad type", () => fresh({ optimization_type: "turbo" }));
refused("bad error code", () => fresh({ ...failed, error_code: "whatever" }));
refused("failed without a code", () => fresh({ ...failed, error_code: null }), "telemetry_failed_has_code");
refused("success with an error", () => fresh({ error_code: "TOO_HOT" }), "telemetry_success_is_clean");
refused("success not saved", () => fresh({ tune_saved: false }));
refused("interrupted with a duration", () => fresh({ ...interrupted, duration_ms: 5 }), "telemetry_interrupted_claims_nothing");
refused("tune on an unsaved run", () => fresh({ ...failed, tune }), "telemetry_tune_only_when_saved");
refused("steps on another type", () => fresh({ ...failed, steps: allSteps }), "telemetry_steps_only_all_in_one");
refused("schema version 2", () => fresh({ schema_version: 2 }));
refused("not NVIDIA", () => fresh({ vendor_id: 4098 }));
refused("negative duration", () => fresh({ duration_ms: -1 }));
refused("control character in name", () => fresh({ gpu_name: "RTX\n4070" }));
refused("score of zero", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, score: 0 } }));
refused("temperature out of range", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, peak_temp_c: 500 } }));
refused("text as a number", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, score: "high" } }));
refused("unknown measurement key", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, username: "x" } }));
refused("measurement without verdict", () => fresh({ stock_measurements: { score: 5 } }));
refused("unknown tune key", () => fresh({ tune: { ...tune, path: "C:/x" } }));
refused("unknown step", () => fresh({ steps: { other: "ok" } }));
refused("step without a value", () => fresh({ steps: { ...allSteps, overclock: null } }));
refused("step with another value", () => fresh({ steps: { ...allSteps, overclock: "maybe" } }));
refused("tune without undervolt_enabled", () => fresh({ tune: { core_offset_mhz: 135 } }));
refused("verdict not a known word", () => fresh({ stock_measurements: { verdict: "fine" } }));
refused("measurement not an object", () => fresh({ stock_measurements: [1, 2] }));

// ---- outcomes
refused("aborted is not a run outcome", () => fresh({ ...cancelled, outcome: "aborted" }));
accepted("aborted stays a measurement verdict", () => fresh({ ...cancelled, stock_measurements: { verdict: "aborted", seconds: 12 } }));
accepted("cancelled after a failed reset to stock", () => fresh({ ...cancelled, stock_restored: false, error_code: "STOCK_RESTORE_FAILED" }));
refused("cancelled blames the card", () => fresh({ ...cancelled, error_code: "TOO_HOT" }), "telemetry_cancelled_blames_nothing");
refused("cancelled names a stage", () => fresh({ ...cancelled, failed_stage: "search" }), "telemetry_cancelled_blames_nothing");
refused("cancelled claims a saved tune", () => fresh({ ...cancelled, tune_saved: true, tune }));
accepted("no_result, could not be saved", () => fresh({ ...noResult, error_code: "SAVE_FAILED", stock_restored: null }));
accepted("not kept and the reset to stock failed", () => fresh({ ...noResult, error_code: "STOCK_RESTORE_FAILED", additional_error_codes: ["NOT_KEPT"], stock_restored: false }));
refused("no_result without a reason", () => fresh({ ...noResult, error_code: null }), "telemetry_no_result_has_reason");
refused("no_result with a technical code", () => fresh({ ...noResult, error_code: "DRIVER_RESET" }), "telemetry_no_result_has_reason");
refused("NOT_KEPT is not a failure", () => fresh({ ...failed, error_code: "NOT_KEPT" }), "telemetry_failed_has_code");
refused("SAVE_FAILED is not a failure", () => fresh({ ...failed, error_code: "SAVE_FAILED" }), "telemetry_failed_has_code");
refused("NOT_KEPT on a retest", () => fresh({ ...noResult, ...retest, failed_stage: null }), "telemetry_not_kept_not_retest");
refused("failed claims a saved tune", () => fresh({ ...failed, tune_saved: true }), "telemetry_saved_means_success");
refused("success without a tune", () => fresh({ tune: null }), "telemetry_success_is_clean");
refused("success without a result", () => fresh({ result_measurements: null }), "telemetry_success_is_clean");
refused("success on an unstable result", () => fresh({ result_measurements: { verdict: "too_hot" } }), "telemetry_success_is_clean");
refused("success says it restored stock", () => fresh({ stock_restored: true }), "telemetry_success_is_clean");
refused("a finished run without a duration", () => fresh({ ...failed, duration_ms: null }), "telemetry_finished_run_is_known");
refused("a finished run with unknown resets", () => fresh({ ...failed, driver_resets: null }), "telemetry_finished_run_is_known");
refused("a finished run with tune_saved unknown", () => fresh({ ...failed, tune_saved: null }), "telemetry_finished_run_is_known");
refused("restore failed without its code", () => fresh({ ...failed, stock_restored: false }), "telemetry_stock_failure_has_code");
refused("its code without the restore failing", () => fresh({ ...failed, error_code: "GPU_UNREACHABLE" }), "telemetry_stock_failure_has_code");
accepted("a run that threw, its reset to stock not known", () => fresh({ ...failed, error_code: "INTERNAL_ERROR", stock_restored: null }));
accepted("GPU unreachable", () => fresh({ ...failed, error_code: "GPU_UNREACHABLE", stock_restored: false, driver_resets: 1 }));

// ---- an interrupted run knows nothing it could not have known
for (
  const [key, value] of Object.entries({
    stock_measurements: { verdict: "stable", seconds: 300 },
    result_measurements: { verdict: "stable", seconds: 300 },
    tune,
    steps: allSteps,
    duration_ms: 1000,
    driver_resets: 0,
    stock_restored: true,
    tune_saved: false,
    failed_stage: "search",
    error_code: "UNCLASSIFIED",
    measurement_disturbed: true,
  })
) {
  refused(`interrupted claims ${key}`, () => fresh({ ...interrupted, [key]: value }));
}

// ---- value types and shapes
refused("JSON null as a measurement object", () => ({ ...fresh(), stock_measurements: "null-json" }));
Deno.test("refused: JSON null stored as a value in a jsonb column", async () => {
  let error = "";
  try {
    await asRole(db, "postgres", () =>
      db.query(
        `insert into public.telemetry_runs (id, installation_id, run_id, schema_version, occurred_at, app_version, app_channel,
           optimization_type, profile, outcome, stock_measurements)
         values ($1, $2, $3, 1, now(), '0.5.0', 'stable', 'quiet', 'quiet', 'interrupted', 'null'::jsonb)`,
        [newId(), newId(), newId()],
      ));
  } catch (e) {
    error = (e as Error).message;
  }
  assert(error, "a JSON null went into a measurement column");
});
refused("half a degree", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, peak_temp_c: 67.5 } }));
refused("a fractional offset", () => fresh({ tune: { ...tune, core_offset_mhz: 135.5 } }));
refused("verdict of another JSON type", () => fresh({ stock_measurements: { verdict: 1 } }));
refused("averages without a counted part", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, counted_score: 5 } }));
accepted("averages with their counted part", () => fresh({ stock_measurements: { verdict: "stable", seconds: 300, counted_seconds: 150, counted_score: 5 } }));
refused("undervolt on without its values", () => fresh({ tune: { ...tune, undervolt_mv: null } }));
refused("undervolt off with its values", () => fresh({ tune: { ...tune, undervolt_enabled: false } }));
accepted("a tune without an undervolt", () => fresh({ tune: { ...tune, undervolt_enabled: false, undervolt_mv: null, undervolt_mhz: null } }));
refused("tune without an offset", () => fresh({ tune: { undervolt_enabled: false } }));
refused("steps not all there", () => fresh({ steps: { overclock: "ok" } }));
refused("steps of another JSON type", () => fresh({ steps: { ...allSteps, overclock: 1 } }));
refused("steps as an array", () => fresh({ steps: ["ok"] }));
refused("all in one without steps", () => fresh({ steps: null }), "telemetry_all_in_one_has_steps");
refused("a path in the GPU name", () => fresh({ gpu_name: "C:\\Users\\someone" }));
refused("an address in the GPU name", () => fresh({ gpu_name: "someone@example.com" }));
refused("trailing newline in the GPU name", () => fresh({ gpu_name: "RTX 4070\n" }));
refused("unknown architecture", () => fresh({ architecture: "Someone" }));
refused("stable channel on a suffixed version", () => fresh({ app_version: "0.5.0-rc1" }), "telemetry_channel_fits_version");
refused("prerelease channel on a plain version", () => fresh({ app_channel: "prerelease" }), "telemetry_channel_fits_version");

// ---- what a kind of run can have
refused("profile differs from the type", () => fresh({ profile: "quiet" }), "telemetry_profile_fits_type");
accepted("undervolt on a saved All in one tune", () =>
  fresh({ ...failed, on_saved_tune: true, profile: "all_in_one", stock_measurements: null }));
refused("on a saved tune with a stock measurement", () => fresh({ ...failed, on_saved_tune: true, profile: "all_in_one" }), "telemetry_saved_tune_has_no_stock");
refused("on_saved_tune on another type", () => fresh({ ...failed, optimization_type: "quiet", profile: "quiet", on_saved_tune: true, stock_measurements: null }), "telemetry_saved_tune_has_no_stock");
accepted("a retest that failed", () => fresh({ ...failed, ...retest, failed_stage: "retest" }));
refused("a retest with a stock measurement", () => fresh({ ...failed, ...retest, stock_measurements: example.stock_measurements, failed_stage: "retest" }), "telemetry_retest_has_no_stock");
refused("retest stage on another run", () => fresh({ ...failed, failed_stage: "retest" }), "telemetry_stage_fits_type");
refused("another stage on a retest", () => fresh({ ...failed, ...retest, failed_stage: "search" }), "telemetry_stage_fits_type");
refused("fan stage outside All in one", () => fresh({ ...failed, failed_stage: "fan_tune" }), "telemetry_stage_fits_type");
refused("fan failure outside the fan step", () => fresh({ ...failed, error_code: "FAN_CONTROL_FAILED" }), "telemetry_fan_failure_in_fan_step");

// ---- what the run code can really produce
const aioFailed: Row = {
  ...failed, optimization_type: "all_in_one", profile: "all_in_one",
  steps: { overclock: "ok", undervolt: "ok", fan_tune: "ok", final_test: "failed" },
  failed_stage: "final_test", error_code: "FAN_CONTROL_FAILED",
};
refused("stable with no time: a measurement that never ran", () => fresh({ stock_measurements: { verdict: "stable", seconds: 0 } }));
refused("stable with unknown time", () => fresh({ stock_measurements: { verdict: "stable", seconds: null } }));
refused("a counted part on a run that did not hold", () => fresh({ ...failed, stock_measurements: { verdict: "too_hot", seconds: 30, counted_seconds: 15 } }));
accepted("a failed retest that saved a lower step", () => fresh({ ...failed, ...retest, failed_stage: "retest", tune_saved: true, tune }));
accepted("a stopped retest that saved a lower step", () => fresh({ ...cancelled, ...retest, tune_saved: true, tune, stock_restored: null }));
refused("no_result claims a saved retest", () => fresh({ ...noResult, ...retest, error_code: "SAVE_FAILED", stock_restored: null, tune_saved: true, tune }), "telemetry_save_failure_saved_nothing");
refused("no_result claims a saved tune", () => fresh({ ...noResult, tune_saved: true, tune }), "telemetry_saved_means_success");
accepted("cancelled with an internal error", () => fresh({ ...cancelled, error_code: "INTERNAL_ERROR" }));
refused("a failed reset with an ordinary code", () => fresh({ ...cancelled, stock_restored: false }), "telemetry_stock_failure_has_code");
accepted("All in one, fan failure in the final test", () => fresh(aioFailed));
accepted("All in one, fan failure in the fan step", () => fresh({ ...aioFailed, failed_stage: "fan_tune" }));
refused("fan failure with no stage", () => fresh({ ...aioFailed, failed_stage: null }), "telemetry_fan_failure_in_fan_step");

// ---- no free text in the fields that are text
for (const name of ["NVIDIA GeForce RTX 4070", "GeForce GTX 1060 6GB", "NVIDIA TITAN Xp", "TITAN X (Pascal)", "Quadro P2000", "Tesla P100", "NVIDIA RTX A4000", "NVIDIA GeForce RTX 4080 SUPER Laptop GPU"]) {
  accepted(`card name: ${name}`, () => fresh({ gpu_name: name }));
}
for (const name of ["Jan de Vries", "DESKTOP-EXAMPLE", "my computer", "GPU-1234", "NVIDIA", `NVIDIA ${"x".repeat(60)}`]) {
  refused(`not a card name: ${name}`, () => fresh({ gpu_name: name }));
}
for (const v of ["Windows 10", "Windows 11", "Windows Server 2022"]) accepted(`Windows version: ${v}`, () => fresh({ windows_version: v }));
for (const v of ["Windows Jan de Vries", "Windows 11 of Jan", "Windows 12345", "Windows 11\n"]) {
  refused(`not a Windows version: ${v}`, () => fresh({ windows_version: v }));
}
for (const v of ["0.5.0-rc1", "0.4.10-check112", "0.5.0-test2", "0.5.0-dev1"]) {
  accepted(`version: ${v}`, () => fresh({ app_version: v, app_channel: "prerelease" }));
}
for (const v of ["0.5.0-janDeVries", "0.5.0-rc", "0.5.0-C.Users.jan", "0.5.0-rc1\n"]) {
  refused(`not a version: ${v}`, () => fresh({ app_version: v, app_channel: "prerelease" }));
}

// ---- several failures in one run
const more = (error_code: string | null, ...additional_error_codes: string[]) => ({ error_code, additional_error_codes });
const stockFailed = { ...failed, stock_restored: false };

accepted("exception and failed reset: the reset comes first", () => fresh({ ...stockFailed, ...more("STOCK_RESTORE_FAILED", "INTERNAL_ERROR") }));
refused("exception hides a failed reset", () => fresh({ ...stockFailed, ...more("INTERNAL_ERROR") }), "telemetry_stock_failure_has_code");
refused("exception first, failed reset second", () => fresh({ ...stockFailed, ...more("INTERNAL_ERROR", "STOCK_RESTORE_FAILED") }));
refused("a failed reset only in the additional codes", () => fresh({ ...failed, ...more("TOO_HOT", "STOCK_RESTORE_FAILED") }));
refused("a failed reset claimed while the card is at stock", () => fresh({ ...failed, ...more("STOCK_RESTORE_FAILED", "TOO_HOT") }), "telemetry_stock_failure_has_code");
refused("unreachable and reset failed together", () => fresh({ ...stockFailed, ...more("GPU_UNREACHABLE", "STOCK_RESTORE_FAILED") }), "telemetry_stock_failure_has_code");
accepted("exception with the reset state unknown", () => fresh({ ...failed, stock_restored: null, ...more("INTERNAL_ERROR") }));
accepted("unreachable keeps the cause of the run", () => fresh({ ...stockFailed, driver_resets: 1, ...more("GPU_UNREACHABLE", "DRIVER_RESET") }));
accepted("every cause at once, in order", () =>
  fresh({ ...stockFailed, ...more("STOCK_RESTORE_FAILED", "INTERNAL_ERROR", "TOO_HOT", "DRIVER_RESET", "STABILITY_CHECK_FAILED", "MEASUREMENT_FAILED", "SAVE_FAILED") }));

accepted("failed, and its lower step could not be saved", () => fresh({ ...failed, ...retest, failed_stage: "retest", ...more("TOO_HOT", "SAVE_FAILED") }));
accepted("cancelled, and the old tune could not be put back", () => fresh({ ...cancelled, ...more("SAVE_FAILED") }));
accepted("cancelled, failed reset and a failed save", () => fresh({ ...cancelled, stock_restored: false, ...more("STOCK_RESTORE_FAILED", "SAVE_FAILED") }));
accepted("no_result: failed save and failed reset", () => fresh({ ...noResult, stock_restored: false, ...more("STOCK_RESTORE_FAILED", "SAVE_FAILED") }));
accepted("no_result: unreachable and a failed save", () => fresh({ ...noResult, stock_restored: false, driver_resets: 1, ...more("GPU_UNREACHABLE", "SAVE_FAILED") }));
refused("no_result with only a failed reset", () => fresh({ ...noResult, stock_restored: false, ...more("STOCK_RESTORE_FAILED") }), "telemetry_no_result_has_reason");
refused("no_result with a technical cause next to its reason", () => fresh({ ...noResult, ...more("TOO_HOT", "SAVE_FAILED") }), "telemetry_no_result_has_reason");
refused("SAVE_FAILED on a run that saved", () => fresh({ ...failed, ...retest, failed_stage: "retest", tune_saved: true, tune, ...more("TOO_HOT", "SAVE_FAILED") }), "telemetry_save_failure_saved_nothing");
refused("SAVE_FAILED as the code of a failed run", () => fresh({ ...failed, ...more("SAVE_FAILED") }), "telemetry_failed_has_code");
refused("NOT_KEPT next to a failure", () => fresh({ ...failed, ...more("TOO_HOT", "NOT_KEPT") }), "telemetry_failed_has_code");
refused("NOT_KEPT next to a retest's reset", () => fresh({ ...noResult, ...retest, stock_restored: false, ...more("STOCK_RESTORE_FAILED", "NOT_KEPT") }), "telemetry_not_kept_not_retest");
refused("cancelled blames the card in an additional code", () => fresh({ ...cancelled, ...more("INTERNAL_ERROR", "TOO_HOT") }), "telemetry_cancelled_blames_nothing");
refused("a fan failure outside All in one, as an additional code", () => fresh({ ...failed, ...more("TOO_HOT", "FAN_CONTROL_FAILED") }), "telemetry_fan_failure_in_fan_step");
accepted("All in one: hot and a fan failure", () => fresh({ ...aioFailed, ...more("TOO_HOT", "FAN_CONTROL_FAILED") }));

refused("additional codes without a first code", () => fresh({ ...cancelled, ...more(null, "SAVE_FAILED") }), "telemetry_error_set");
refused("additional codes on a success", () => fresh(more(null, "SAVE_FAILED")), "telemetry_error_set");
refused("additional codes on an interrupted run", () => fresh({ ...interrupted, ...more(null, "UNCLASSIFIED") }), "telemetry_error_set");
refused("a code twice", () => fresh({ ...failed, ...more("TOO_HOT", "DRIVER_RESET", "DRIVER_RESET") }), "telemetry_error_set");
refused("the first code repeated", () => fresh({ ...failed, ...more("TOO_HOT", "TOO_HOT") }), "telemetry_error_set");
refused("additional codes out of order", () => fresh({ ...failed, ...more("TOO_HOT", "MEASUREMENT_FAILED", "DRIVER_RESET") }), "telemetry_error_set");
refused("a first code below an additional one", () => fresh({ ...failed, ...more("DRIVER_RESET", "TOO_HOT") }), "telemetry_error_set");
refused("an unknown additional code", () => fresh({ ...failed, ...more("TOO_HOT", "C:/Users/someone") }), "telemetry_error_set");
refused("free text as an additional code", () => fresh({ ...failed, ...more("TOO_HOT", "unexpected error: boom") }), "telemetry_error_set");
refused("a null additional code", () => fresh({ ...failed, error_code: "TOO_HOT", additional_error_codes: ["DRIVER_RESET", null] }), "telemetry_error_set");
refused("no additional codes value", () => fresh({ ...failed, additional_error_codes: null }));
refused("UNCLASSIFIED next to a known cause", () => fresh({ ...failed, ...more("TOO_HOT", "UNCLASSIFIED") }), "telemetry_unclassified_is_alone");
accepted("UNCLASSIFIED next to a failed reset", () => fresh({ ...stockFailed, ...more("STOCK_RESTORE_FAILED", "UNCLASSIFIED") }));
accepted("UNCLASSIFIED with a failed save", () => fresh({ ...failed, ...more("UNCLASSIFIED", "SAVE_FAILED") }));

Deno.test("the order of the codes puts the state of the card first", async () => {
  const r = await db.query<{ c: string[] }>("select public.telemetry_error_codes() as c");
  assertEq(r.rows[0].c.slice(0, 3), ["GPU_UNREACHABLE", "STOCK_RESTORE_FAILED", "INTERNAL_ERROR"]);
  assertEq(r.rows[0].c.length, 11);
  assertEq(new Set(r.rows[0].c).size, 11);
});

// ---- access
for (const role of ["anon", "authenticated"]) {
  Deno.test(`denied: ${role} insert`, async () => {
    let error = "";
    try {
      await insert(fresh(), role);
    } catch (e) {
      error = (e as Error).message;
    }
    assert(error.includes("permission denied"), `got: ${error}`);
  });
  for (
    const q of [
      "select * from public.telemetry_runs",
      "delete from public.telemetry_runs",
      "update public.telemetry_runs set gpu_name = 'x'",
      "truncate public.telemetry_runs",
      "select public.telemetry_valid_tune('{}')",
      "select * from public.telemetry_rate_limits",
      "insert into public.telemetry_rate_limits values ('submit:global', now(), 1)",
      "select public.telemetry_submit('{}')",
      "select public.telemetry_delete('00000000-0000-4000-8000-000000000001')",
      "select public.telemetry_take('submit:global', now(), 10)",
      "select public.telemetry_purge_expired()",
    ]
  ) {
    Deno.test(`denied: ${role}: ${q}`, async () => {
      let error = "";
      try {
        await asRole(db, role, () => db.query(q));
      } catch (e) {
        error = (e as Error).message;
      }
      assert(error.includes("permission denied"), `got: ${error}`);
    });
  }
}
// A leaked service key can call the two entry points and nothing else.
for (
  const q of [
    "select * from public.telemetry_runs",
    "insert into public.telemetry_runs (id) values ('00000000-0000-4000-8000-000000000001')",
    "update public.telemetry_runs set gpu_name = 'x'",
    "delete from public.telemetry_runs",
    "truncate public.telemetry_runs",
    "select * from public.telemetry_rate_limits",
    "insert into public.telemetry_rate_limits values ('submit:global', now(), 1)",
    "update public.telemetry_rate_limits set hits = 0",
    "truncate public.telemetry_rate_limits",
    "delete from public.telemetry_rate_limits",
    "select public.telemetry_purge_expired()",
    "select public.telemetry_take('submit:global', now(), 10)",
    "select public.telemetry_admit('submit', '00000000-0000-4000-8000-000000000001', 60, 1000)",
    "select public.telemetry_id_bucket('submit', '00000000-0000-4000-8000-000000000001')",
    "select public.telemetry_valid_tune('{}')",
    "select public.telemetry_error_codes()",
  ]
) {
  Deno.test(`denied: service_role: ${q}`, async () => {
    let error = "";
    try {
      await asRole(db, "service_role", () => db.query(q));
    } catch (e) {
      error = (e as Error).message;
    }
    assert(error.includes("permission denied"), `got: ${error}`);
  });
}
Deno.test("the service role can call the two entry points", async () => {
  const r = await asRole(db, "service_role", () =>
    db.query<{ s: string; d: string }>(
      "select public.telemetry_submit('{}') as s, public.telemetry_delete('00000000-0000-4000-8000-0000000000aa') as d",
    ));
  assertEq(r.rows[0], { s: "invalid", d: "deleted" });
});
// With an empty search_path, pg_temp is searched first for types. A caller's
// temporary domain named like a built-in type would then run its CHECK as the owner.
for (const type of ["uuid", "text", "timestamptz"]) {
  Deno.test(`a caller's temporary ${type} cannot run code in an entry point`, async () => {
    const row = JSON.stringify(fresh());
    let error = "";
    await db.exec(`set role service_role;
      create function pg_temp.spy(v pg_catalog.text) returns boolean language plpgsql as $f$
        begin raise exception 'ran as %', current_user; end $f$;
      create domain pg_temp.${type} as pg_catalog.${type} check (pg_temp.spy(value::pg_catalog.text));`);
    try {
      await db.query("select public.telemetry_submit($1::pg_catalog.jsonb)", [row]);
      await db.query("select public.telemetry_delete('00000000-0000-4000-8000-0000000000bb'::pg_catalog.uuid)");
    } catch (e) {
      error = (e as Error).message;
    } finally {
      await db.exec(`drop domain pg_temp.${type}; drop function pg_temp.spy(pg_catalog.text); reset role`);
    }
    assertEq(error, "");
  });
}
Deno.test("the access model holds in the catalog", async () => {
  const r = await db.query<{ problem: string }>(auditSql());
  assertEq(r.rows.map((x) => x.problem), []);
});
Deno.test("the audit sees what it is there to find", async () => {
  const breaks: [string, string, string][] = [
    ["grant select on public.telemetry_runs to anon", "revoke select on public.telemetry_runs from anon", "SELECT on telemetry_runs for anon"],
    ["grant update on public.telemetry_rate_limits to service_role", "revoke update on public.telemetry_rate_limits from service_role", "UPDATE on telemetry_rate_limits for service_role"],
    ["alter table public.telemetry_runs disable row level security", "alter table public.telemetry_runs enable row level security", "no row level security: telemetry_runs"],
    ["create policy p on public.telemetry_runs using (true)", "drop policy p on public.telemetry_runs", "a policy on telemetry_runs: p"],
    ["alter function public.telemetry_purge_expired() security definer", "alter function public.telemetry_purge_expired() security invoker", "SECURITY DEFINER: telemetry_purge_expired"],
    ["alter function public.telemetry_submit(jsonb) set search_path = ''", "alter function public.telemetry_submit(jsonb) set search_path = pg_catalog, pg_temp", "search_path not safe: telemetry_submit"],
    ["grant execute on function public.telemetry_take(text, timestamptz, integer) to service_role", "revoke execute on function public.telemetry_take(text, timestamptz, integer) from service_role", "EXECUTE on function telemetry_take for service_role"],
    ["grant execute on function public.telemetry_submit(jsonb) to anon", "revoke execute on function public.telemetry_submit(jsonb) from anon", "EXECUTE on function telemetry_submit for anon"],
  ];
  for (const [breakIt, undo, expected] of breaks) {
    await db.exec(breakIt);
    const r = await db.query<{ problem: string }>(auditSql());
    await db.exec(undo);
    assert(r.rows.some((x) => x.problem === expected), `"${expected}" not in ${JSON.stringify(r.rows)}`);
  }
  assertEq((await db.query(auditSql())).rows, [], "everything undone");
});
Deno.test("row level security is on, without a policy", async () => {
  const r = await db.query<{ relname: string; relrowsecurity: boolean }>(
    "select relname, relrowsecurity from pg_class where relname in ('telemetry_runs', 'telemetry_rate_limits') order by 1",
  );
  assertEq(r.rows.map((x) => x.relrowsecurity), [true, true]);
  const p = await db.query<{ c: number }>("select count(*)::int as c from pg_policies where tablename like 'telemetry%'");
  assertEq(p.rows[0].c, 0);
});
Deno.test("every telemetry function has a fixed search_path", async () => {
  const r = await db.query<{ proname: string }>(
    `select proname from pg_proc p join pg_namespace n on n.oid = p.pronamespace
     where n.nspname = 'public' and proname like 'telemetry%' and not coalesce(proconfig::text like '%search_path%', false)`,
  );
  assertEq(r.rows, []);
});
