// The database on its own: constraints, validators, grants. The cases of the
// first review (phase A) come first; they are kept, with a whole `steps` object
// where the validator now requires one.
import { toRow } from "../functions/_shared/telemetry.ts";
import { asRole, assert, assertEq, docExample, freshDb, newId, type Row } from "./helpers.ts";

const db = await freshDb();
const example = toRow(docExample());
const jsonCols = new Set(["stock_measurements", "result_measurements", "tune", "steps"]);

async function insert(r: Row, role = "service_role") {
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
accepted("not kept and the reset to stock failed", () => fresh({ ...noResult, error_code: "STOCK_RESTORE_FAILED", stock_restored: false }));
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
    await asRole(db, "service_role", () =>
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
refused("no_result claims a saved retest", () => fresh({ ...noResult, ...retest, error_code: "SAVE_FAILED", stock_restored: null, tune_saved: true, tune }), "telemetry_saved_means_success");
accepted("internal error with a failed reset to stock", () => fresh({ ...failed, error_code: "INTERNAL_ERROR", stock_restored: false }));
accepted("cancelled with an internal error", () => fresh({ ...cancelled, error_code: "INTERNAL_ERROR" }));
refused("a failed reset with an ordinary code", () => fresh({ ...cancelled, stock_restored: false }), "telemetry_stock_failure_has_code");
accepted("All in one, fan failure in the final test", () => fresh(aioFailed));
accepted("All in one, fan failure in the fan step", () => fresh({ ...aioFailed, failed_stage: "fan_tune" }));
refused("fan failure with no stage", () => fresh({ ...aioFailed, failed_stage: null }), "telemetry_fan_failure_in_fan_step");

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
for (
  const q of [
    "update public.telemetry_runs set gpu_name = 'x'",
    "truncate public.telemetry_runs",
    "truncate public.telemetry_rate_limits",
    "delete from public.telemetry_rate_limits",
    "select public.telemetry_purge_expired()",
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
Deno.test("service role reads the table", async () => {
  const r = await asRole(db, "service_role", () => db.query<{ c: number }>("select count(*)::int as c from public.telemetry_runs"));
  assert(r.rows[0].c > 0, "no rows");
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
