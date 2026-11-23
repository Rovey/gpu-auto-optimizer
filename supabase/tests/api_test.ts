// The two endpoints through to the database, with synthetic events.
import { handleDelete, handleSubmit, MAX_BODY_BYTES, restRpc, serviceKey } from "../functions/_shared/telemetry.ts";
import { assert, assertEq, count, docExample, freshDb, migrationFiles, newId, readMigration, type Row, serviceRpc } from "./helpers.ts";

const db = await freshDb();
const rpc = serviceRpc(db);
const now = Date.now();
const stamp = (ms: number) => new Date(Math.floor(ms / 1000) * 1000).toISOString().replace(".000Z", "Z");

type Event = Row & { optimization: Row; measurements: Row; gpu: Row };
function event(change: (e: Event) => void = () => {}): Event {
  const e = docExample() as Event;
  e.event_id = newId();
  e.run_id = newId();
  e.installation_id = newId();
  e.occurred_at = stamp(now);
  change(e);
  return e;
}
const post = (body: string, headers: Record<string, string> = { "content-type": "application/json" }) =>
  new Request("http://localhost/fn", { method: "POST", headers, body });
const submit = async (e: unknown) => {
  const r = await handleSubmit(post(JSON.stringify(e)), rpc, now);
  return { status: r.status, body: await r.json() };
};
const remove = async (id: unknown) => {
  const r = await handleDelete(post(JSON.stringify({ installation_id: id })), rpc);
  return { status: r.status, body: await r.json() };
};
const accepted = { status: 200, body: { accepted: true, duplicate: false } };
const duplicate = { status: 200, body: { accepted: true, duplicate: true } };
const invalid = { status: 400, body: { accepted: false, error: "INVALID_EVENT" } };
const limited = { status: 429, body: { accepted: false, error: "RATE_LIMITED" } };
const clearLimits = () => db.exec("delete from public.telemetry_rate_limits");

function asFailed(e: Event) {
  Object.assign(e.optimization, {
    type: "undervolt", profile: "undervolt", outcome: "failed", failed_stage: "stock", error_code: "TOO_HOT",
    tune_saved: false, stock_restored: true, steps: null,
  });
  e.measurements.result = null;
  e.tune = null;
}
function asInterrupted(e: Event) {
  Object.assign(e.optimization, {
    outcome: "interrupted", duration_ms: null, driver_resets: null, stock_restored: null, tune_saved: null, steps: null,
  });
  e.measurements.stock = null;
  e.measurements.result = null;
  e.tune = null;
}

// 1
Deno.test("a valid event is accepted and stored", async () => {
  const e = event();
  assertEq(await submit(e), accepted);
  const r = await db.query<Row>("select gpu_name, outcome, tune, received_at from public.telemetry_runs where id = $1", [e.event_id]);
  assertEq(r.rows[0].gpu_name, "NVIDIA GeForce RTX 4070");
  assertEq(r.rows[0].outcome, "success");
  assertEq((r.rows[0].tune as Row).undervolt_mv, 1035);
});
// 2
Deno.test("an invalid event is refused and stores nothing", async () => {
  const e = event((x) => (x.optimization.outcome = "great"));
  assertEq(await submit(e), invalid);
  assertEq(await count(db, "id = $1", [e.event_id]), 0);
});
Deno.test("what is not a JSON event is refused", async () => {
  for (const body of ["", "{", "[]", "null", "1", '"x"', "{}"]) {
    const r = await handleSubmit(post(body), rpc, now);
    assertEq({ status: r.status, body: await r.json() }, invalid, `body ${body}`);
  }
  const bytes = new Request("http://localhost/fn", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: new Uint8Array([0x7b, 0xff, 0x7d]),
  });
  assertEq((await handleSubmit(bytes, rpc, now)).status, 400, "not UTF-8");
  assertEq((await handleSubmit(post("{}", { "content-type": "text/plain" }), rpc, now)).status, 400, "content type");
  const get = await handleSubmit(new Request("http://localhost/fn"), rpc, now);
  assertEq(get.status, 405);
  await get.body?.cancel();
  const options = await handleSubmit(new Request("http://localhost/fn", { method: "OPTIONS" }), rpc, now);
  assertEq(options.status, 405, "no CORS: browsers are not clients");
  assertEq(options.headers.get("access-control-allow-origin"), null);
  await options.body?.cancel();
});
// 3
Deno.test("an unknown field is refused, at every level", async () => {
  const changes: ((e: Event) => void)[] = [
    (e) => (e.username = "x"),
    (e) => ((e.application as Row).path = "C:/x"),
    (e) => ((e.system as Row).computer_name = "x"),
    (e) => (e.gpu.uuid = "GPU-1"),
    (e) => (e.gpu.pci_bus_id = "0000:01:00.0"),
    (e) => ((e.driver as Row).extra = 1),
    (e) => (e.optimization.error_text = "boom"),
    (e) => (e.optimization.process = "game.exe"),
    (e) => ((e.optimization.steps as Row).other = "ok"),
    (e) => (e.measurements.extra = null),
    (e) => ((e.measurements.stock as Row).log = "x"),
    (e) => ((e.tune as Row).path = "x"),
  ];
  for (const change of changes) assertEq(await submit(event(change)), invalid);
  const proto = JSON.stringify(event()).replace('{"schema_version"', '{"__proto__":{"x":1},"schema_version"');
  assertEq((await handleSubmit(post(proto), rpc, now)).status, 400, "__proto__");
});
Deno.test("a missing field is refused: not measured is an explicit null", async () => {
  assertEq(await submit(event((e) => delete e.gpu.name)), invalid);
  assertEq(await submit(event((e) => delete (e.measurements.stock as Row).score)), invalid);
  assertEq(await submit(event((e) => delete e.tune)), invalid);
});
// 4
Deno.test("invalid numbers are refused", async () => {
  const changes: ((e: Event) => void)[] = [
    (e) => ((e.measurements.stock as Row).score = 0),
    (e) => ((e.measurements.stock as Row).score = -1),
    (e) => ((e.measurements.stock as Row).score = "5728"),
    (e) => ((e.measurements.stock as Row).peak_temp_c = 67.5),
    (e) => ((e.measurements.stock as Row).peak_temp_c = 500),
    (e) => ((e.measurements.stock as Row).end_fan_pct = -1),
    (e) => (e.optimization.duration_ms = -1),
    (e) => (e.optimization.duration_ms = 1.5),
    (e) => (e.optimization.driver_resets = 101),
    (e) => (e.gpu.vendor_id = 4098),
    (e) => (e.gpu.device_id = 70000),
    (e) => ((e.tune as Row).power_limit_pct = 0),
    (e) => (e.schema_version = 2),
  ];
  for (const change of changes) assertEq(await submit(event(change)), invalid);
  const huge = JSON.stringify(event()).replace('"score":5728', '"score":1e999');
  assertEq((await handleSubmit(post(huge), rpc, now)).status, 400, "Infinity");
});
Deno.test("ids must be lowercase random UUIDs", async () => {
  for (const id of ["x", "", "00000000-0000-0000-0000-000000000000", "6F1D3B52-8D7E-4C3A-9F42-1B6A0F3E7C11"]) {
    assertEq(await submit(event((e) => (e.installation_id = id))), invalid, id);
  }
});
// 5
Deno.test("readings that are missing stay null", async () => {
  const e = event((x) => {
    x.gpu.name = null;
    x.gpu.vram_mb = null;
    const s = x.measurements.stock as Row;
    s.score = s.power_w = s.peak_temp_c = s.end_fan_pct = null;
  });
  assertEq(await submit(e), accepted);
  const r = await db.query<{ gpu_name: unknown; vram_mb: unknown; stock_measurements: Row }>(
    "select gpu_name, vram_mb, stock_measurements from public.telemetry_runs where id = $1",
    [e.event_id],
  );
  assertEq(r.rows[0].gpu_name, null);
  assertEq(r.rows[0].vram_mb, null);
  for (const k of ["score", "power_w", "peak_temp_c", "end_fan_pct"]) assertEq(r.rows[0].stock_measurements[k], null, k);
});
// 6
Deno.test("the same event twice is one row, and the client may remove it", async () => {
  const e = event();
  assertEq(await submit(e), accepted);
  assertEq(await submit(e), duplicate);
  assertEq(await count(db, "id = $1", [e.event_id]), 1);
});
Deno.test("a duplicate is answered even when the limit is reached", async () => {
  await clearLimits();
  const e = event();
  assertEq(await submit(e), accepted);
  await db.query(
    "update public.telemetry_rate_limits set hits = 60 where bucket = public.telemetry_id_bucket('submit', $1)",
    [e.installation_id],
  );
  assertEq(await submit(e), duplicate);
  await clearLimits();
});
// 7
Deno.test("a second event for one run creates no row", async () => {
  const e = event();
  assertEq(await submit(e), accepted);
  assertEq(await submit({ ...e, event_id: newId() }), duplicate);
  assertEq(await count(db, "run_id = $1", [e.run_id]), 1);
});
// 8, 9
Deno.test("aborted is refused as an outcome, cancelled is accepted", async () => {
  const stopped = (outcome: string) => (e: Event) => {
    asFailed(e);
    Object.assign(e.optimization, { outcome, failed_stage: null, error_code: null });
  };
  assertEq(await submit(event(stopped("aborted"))), invalid);
  assertEq(await submit(event(stopped("cancelled"))), accepted);
  // As the verdict of the measurement that was stopped, it is a real value.
  const e = event(stopped("cancelled"));
  (e.measurements.stock as Row).verdict = "aborted";
  assertEq(await submit(e), accepted);
});
// 10
Deno.test("an interrupted run cannot claim what it could not know", async () => {
  assertEq(await submit(event(asInterrupted)), accepted);
  const measured = docExample().measurements as Row;
  const claims: ((e: Event) => void)[] = [
    (e) => (e.measurements.stock = measured.stock),
    (e) => (e.measurements.result = measured.result),
    (e) => (e.tune = docExample().tune),
    (e) => (e.optimization.duration_ms = 1000),
    (e) => (e.optimization.driver_resets = 0),
    (e) => (e.optimization.tune_saved = false),
    (e) => (e.optimization.stock_restored = true),
    (e) => (e.optimization.error_code = "UNCLASSIFIED"),
    (e) => (e.optimization.failed_stage = "search"),
    (e) => (e.optimization.measurement_disturbed = true),
    (e) => (e.optimization.steps = { overclock: "ok", undervolt: "ok", fan_tune: "ok", final_test: "ok" }),
  ];
  for (const claim of claims) {
    assertEq(await submit(event((e) => { asInterrupted(e); claim(e); })), invalid);
  }
});
// 11
Deno.test("NOT_KEPT is a reason on no_result, never a failure", async () => {
  const notKept = (outcome: string) => (e: Event) => {
    asFailed(e);
    Object.assign(e.optimization, { outcome, failed_stage: null, error_code: "NOT_KEPT" });
  };
  assertEq(await submit(event(notKept("no_result"))), accepted);
  assertEq(await submit(event(notKept("failed"))), invalid);
  assertEq(await submit(event(notKept("cancelled"))), invalid);
  assertEq(await submit(event(notKept("success"))), invalid);
});
// 12
Deno.test("the limit per installation holds", async () => {
  await clearLimits();
  const id = newId();
  for (let i = 0; i < 60; i++) assertEq(await submit(event((e) => (e.installation_id = id))), accepted, `event ${i}`);
  const over = event((e) => (e.installation_id = id));
  assertEq(await submit(over), limited);
  assertEq(await count(db, "id = $1", [over.event_id]), 0);
  assertEq(await submit(event()), accepted, "another installation goes on");
  await clearLimits();
});
Deno.test("the limit counts on the server, and a yesterday does not count", async () => {
  await clearLimits();
  const e = event();
  await db.query(
    `insert into public.telemetry_rate_limits (bucket, window_start, hits)
     values (public.telemetry_id_bucket('submit', $1), date_trunc('day', now(), 'UTC') - interval '1 day', 60)`,
    [e.installation_id],
  );
  assertEq(await submit(e), accepted);
  await clearLimits();
});
Deno.test("the global limit holds across installations", async () => {
  await clearLimits();
  await db.exec(`insert into public.telemetry_rate_limits values ('submit:global', date_trunc('hour', now(), 'UTC'), 999)`);
  assertEq(await submit(event()), accepted);
  const over = event();
  assertEq(await submit(over), limited);
  assertEq(await count(db, "id = $1", [over.event_id]), 0);
  await clearLimits();
});
Deno.test("an event the database refuses still takes a hit", async () => {
  await clearLimits();
  const id = newId();
  assertEq(await submit(event((e) => { e.installation_id = id; e.optimization.outcome = "great"; })), invalid);
  const r = await db.query<{ hits: number }>(
    "select hits from public.telemetry_rate_limits where bucket = public.telemetry_id_bucket('submit', $1)",
    [id],
  );
  assertEq(r.rows[0]?.hits, 1);
  await clearLimits();
});
Deno.test("counters do not hold the installation ID", async () => {
  await clearLimits();
  const e = event();
  await submit(e);
  const r = await db.query<{ bucket: string }>("select bucket from public.telemetry_rate_limits");
  for (const row of r.rows) assert(!row.bucket.includes(e.installation_id as string), "raw ID in a counter");
  await clearLimits();
});
// ---- the counters stay bounded
const counters = async (where = "true") =>
  (await db.query<{ c: number }>(`select count(*)::int as c from public.telemetry_rate_limits where ${where}`)).rows[0].c;
const hitsOf = async (bucket: string, window: string) =>
  (await db.query<{ hits: number }>(
    `select hits from public.telemetry_rate_limits where bucket = ${bucket} and window_start = date_trunc('${window}', now(), 'UTC')`,
  )).rows[0]?.hits;
const hash = (n: number) => n.toString(16).padStart(64, "0");
// Counters of a window that has ended, or of one that has not.
const seed = (n: number, start: string, first = 1) =>
  db.exec(`insert into public.telemetry_rate_limits
    select 'submit:' || lpad(to_hex(i), 64, '0'), ${start}, 1 from generate_series(${first}, ${first + n - 1}) as i`);
const yesterday = "date_trunc('day', now(), 'UTC') - interval '1 day'";
const today = "date_trunc('day', now(), 'UTC')";

Deno.test("rotating installation IDs add no counter once the hour is full", async () => {
  await clearLimits();
  await db.exec(`insert into public.telemetry_rate_limits values
    ('submit:global', date_trunc('hour', now(), 'UTC'), 1000), ('delete:global', date_trunc('hour', now(), 'UTC'), 600)`);
  for (let i = 0; i < 25; i++) {
    assertEq(await submit(event()), limited);
    assertEq((await remove(newId())).status, 429);
  }
  assertEq(await counters(), 2, "only the two global counters");
  assertEq(await hitsOf("'submit:global'", "hour"), 1000);
  assertEq(await hitsOf("'delete:global'", "hour"), 600);
  await clearLimits();
});
Deno.test("an installation at its limit does not use up the global limit", async () => {
  await clearLimits();
  const id = newId();
  await db.query(
    `insert into public.telemetry_rate_limits values (public.telemetry_id_bucket('submit', $1), date_trunc('day', now(), 'UTC'), 60)`,
    [id],
  );
  for (let i = 0; i < 20; i++) assertEq(await submit(event((e) => (e.installation_id = id))), limited);
  assertEq(await hitsOf("'submit:global'", "hour"), undefined, "no global hit taken");
  await clearLimits();
});
Deno.test("each admitted request removes ended counters, never active ones", async () => {
  await clearLimits();
  await seed(25, yesterday);
  await seed(5, today, 1000);
  assertEq(await submit(event()), accepted);
  assertEq(await counters(`window_start < ${today}`), 15, "ten ended counters gone");
  assertEq(await counters(`window_start >= ${today} and bucket like 'submit:0000000000%'`), 5, "active counters stay");
  assertEq(await submit(event()), accepted);
  assertEq(await submit(event()), accepted);
  assertEq(await counters(`window_start < ${today}`), 0, "drained");
  assertEq(await counters(`window_start >= ${today} and bucket like 'submit:0000000000%'`), 5, "active counters still there");
  await clearLimits();
});
Deno.test("a refused request removes nothing and adds nothing", async () => {
  await clearLimits();
  await seed(5, yesterday);
  await db.exec(`insert into public.telemetry_rate_limits values ('submit:global', date_trunc('hour', now(), 'UTC'), 1000)`);
  const before = await counters();
  assertEq(await submit(event()), limited);
  assertEq(await counters(), before);
  await clearLimits();
});
Deno.test("the boundary of a counter is the end of its window", async () => {
  await clearLimits();
  const mine = newId();
  const bucket = `public.telemetry_id_bucket('submit', '${mine}')`;
  await db.exec(`insert into public.telemetry_rate_limits values
    ('submit:${hash(1)}', ${today} - interval '1 microsecond', 1),
    ('submit:${hash(2)}', ${today}, 1),
    ('delete:global', ${today} - interval '1 hour', 1),
    ('delete:global', date_trunc('hour', now(), 'UTC'), 1),
    (${bucket}, ${today}, 59)`);
  await db.exec("select public.telemetry_purge_expired()");
  assertEq(await counters(`bucket = 'submit:${hash(1)}'`), 0, "ended just before today");
  assertEq(await counters(`bucket = 'submit:${hash(2)}'`), 1, "started at the start of today");
  assertEq(await counters(`bucket = 'delete:global' and window_start < ${today}`), 0, "an hour of yesterday");
  assertEq(await counters(`bucket = 'delete:global' and window_start >= ${today}`), 1, "the current hour");
  // The installation's count survives the cleanup, so the cleanup cannot reset its limit.
  assertEq(await submit(event((e) => (e.installation_id = mine))), accepted);
  assertEq(await submit(event((e) => (e.installation_id = mine))), limited, "60 today, also after the purge");
  await clearLimits();
});
Deno.test("the purge removes every ended counter at once", async () => {
  await clearLimits();
  await seed(500, yesterday);
  await db.exec("select public.telemetry_purge_expired()");
  assertEq(await counters(), 0);
});
// ---- several failures, through the endpoint
Deno.test("simultaneous failures arrive in full and in order", async () => {
  await clearLimits();
  const e = event((x) => {
    asFailed(x);
    Object.assign(x.optimization, {
      stock_restored: false, error_code: "STOCK_RESTORE_FAILED", additional_error_codes: ["INTERNAL_ERROR", "TOO_HOT", "SAVE_FAILED"],
    });
  });
  assertEq(await submit(e), accepted);
  const r = await db.query<{ error_code: string; additional_error_codes: string[] }>(
    "select error_code, additional_error_codes from public.telemetry_runs where id = $1",
    [e.event_id],
  );
  assertEq(r.rows[0], { error_code: "STOCK_RESTORE_FAILED", additional_error_codes: ["INTERNAL_ERROR", "TOO_HOT", "SAVE_FAILED"] });
  await clearLimits();
});
Deno.test("additional codes are refused unless they are a list of known codes", async () => {
  await clearLimits();
  const withCodes = (v: unknown) => event((x) => {
    asFailed(x);
    x.optimization.additional_error_codes = v;
  });
  for (const bad of [null, "SAVE_FAILED", {}, [1], [null], ["free text"], ["SAVE_FAILED", "SAVE_FAILED"], Array(11).fill("SAVE_FAILED"), ["x".repeat(129)]]) {
    assertEq(await submit(withCodes(bad)), invalid, JSON.stringify(bad));
  }
  assertEq(await submit(event((x) => delete x.optimization.additional_error_codes)), invalid, "the key is required");
  assertEq(await submit(withCodes(["SAVE_FAILED"])), accepted);
  await clearLimits();
});
Deno.test("an internal error cannot hide a failed reset to stock", async () => {
  await clearLimits();
  const hidden = event((x) => {
    asFailed(x);
    Object.assign(x.optimization, { stock_restored: false, error_code: "INTERNAL_ERROR" });
  });
  assertEq(await submit(hidden), invalid);
  assertEq(await count(db, "id = $1", [hidden.event_id]), 0);
  await clearLimits();
});
// 13
Deno.test("an oversized request is refused", async () => {
  const big = JSON.stringify({ ...event(), padding: "x".repeat(MAX_BODY_BYTES) });
  const r = await handleSubmit(post(big), rpc, now);
  assertEq({ status: r.status, body: await r.json() }, { status: 413, body: { accepted: false, error: "TOO_LARGE" } });
  // A stream with no or a false Content-Length is counted as it is read.
  const chunk = new Uint8Array(4096).fill(0x20);
  let sent = 0;
  const stream = new ReadableStream<Uint8Array>({
    pull(c) {
      if (sent++ > 100) c.close();
      else c.enqueue(chunk);
    },
  });
  const streamed = await handleSubmit(
    new Request("http://localhost/fn", { method: "POST", headers: { "content-type": "application/json" }, body: stream }),
    rpc,
    now,
  );
  assertEq(streamed.status, 413);
  await streamed.body?.cancel();
  assert(sent < 10, `read ${sent} chunks of an oversized body`);
  const del = await handleDelete(post("x".repeat(MAX_BODY_BYTES + 1)), rpc);
  assertEq(del.status, 413);
  await del.body?.cancel();
});
// 14
Deno.test("old and future timestamps are refused", async () => {
  const day = 24 * 60 * 60 * 1000;
  const at = (ms: number) => event((e) => (e.occurred_at = stamp(ms)));
  assertEq(await submit(at(now - 30 * day - 2000)), invalid, "just over 30 days");
  assertEq(await submit(at(now + 5 * 60 * 1000 + 2000)), invalid, "just over 5 minutes ahead");
  assertEq(await submit(at(now - 30 * day + 2000)), accepted, "just under 30 days");
  assertEq(await submit(at(now + 5 * 60 * 1000 - 2000)), accepted, "just under 5 minutes ahead");
  for (const bad of ["2026-02-31T10:00:00Z", "2026-11-23 18:30:00", "2026-11-23T18:30:00+01:00", "2026-11-23T18:30:00.000Z", "yesterday", 5]) {
    assertEq(await submit(event((e) => (e.occurred_at = bad))), invalid, String(bad));
  }
});
// 15, 16
Deno.test("deletion removes one installation and nothing else", async () => {
  await clearLimits();
  const mine = newId(), other = newId();
  for (const id of [mine, mine, other]) assertEq(await submit(event((e) => (e.installation_id = id))), accepted);
  assertEq(await remove(mine), { status: 200, body: { deleted: true } });
  assertEq(await count(db, "installation_id = $1", [mine]), 0);
  assertEq(await count(db, "installation_id = $1", [other]), 1);
  await clearLimits();
});
Deno.test("deletion answers the same whether or not the installation exists", async () => {
  await clearLimits();
  const known = newId();
  assertEq(await submit(event((e) => (e.installation_id = known))), accepted);
  const a = await handleDelete(post(JSON.stringify({ installation_id: known })), rpc);
  const b = await handleDelete(post(JSON.stringify({ installation_id: newId() })), rpc);
  assertEq(a.status, b.status);
  assertEq(await a.text(), await b.text());
  assertEq([...a.headers].sort(), [...b.headers].sort());
  await clearLimits();
});
Deno.test("deletion refuses anything but one UUID", async () => {
  for (const body of ['{"installation_id":"*"}', "{}", '{"installation_id":"x","or":"1=1"}', '{"installation_id":null}', "[]"]) {
    const r = await handleDelete(post(body), rpc);
    assertEq({ status: r.status, body: await r.json() }, { status: 400, body: { deleted: false, error: "INVALID_REQUEST" } }, body);
  }
  const injection = "00000000-0000-4000-8000-000000000001' or '1'='1";
  assertEq((await remove(injection)).status, 400);
  assert((await count(db)) > 0, "rows are still there");
});
Deno.test("deletion is rate limited, per installation and in total", async () => {
  await clearLimits();
  const id = newId();
  for (let i = 0; i < 10; i++) assertEq((await remove(id)).status, 200);
  assertEq(await remove(id), { status: 429, body: { deleted: false, error: "RATE_LIMITED" } });
  await clearLimits();
  await db.exec(`insert into public.telemetry_rate_limits values ('delete:global', date_trunc('hour', now(), 'UTC'), 600)`);
  assertEq((await remove(newId())).status, 429);
  await clearLimits();
});
Deno.test("a limited deletion removes nothing", async () => {
  await clearLimits();
  const id = newId();
  assertEq(await submit(event((e) => (e.installation_id = id))), accepted);
  await db.query(
    `insert into public.telemetry_rate_limits values (public.telemetry_id_bucket('delete', $1), date_trunc('day', now(), 'UTC'), 10)`,
    [id],
  );
  assertEq((await remove(id)).status, 429);
  assertEq(await count(db, "installation_id = $1", [id]), 1);
  await clearLimits();
});
// 19
Deno.test("database errors never reach the client or the log", async () => {
  const secret = "relation telemetry_runs password=hunter2";
  const failing = () => Promise.reject(new Error(secret));
  const logged: unknown[][] = [];
  const original = console.error;
  console.error = (...args: unknown[]) => logged.push(args);
  try {
    const e = event();
    const r = await handleSubmit(post(JSON.stringify(e)), failing, now);
    assertEq(r.status, 500);
    const text = await r.text();
    assertEq(text, '{"accepted":false,"error":"UNAVAILABLE"}');
    const d = await handleDelete(post(JSON.stringify({ installation_id: e.installation_id })), failing);
    assertEq(d.status, 500);
    assertEq(await d.text(), '{"deleted":false,"error":"UNAVAILABLE"}');
    const log = JSON.stringify(logged);
    for (const value of [secret, e.installation_id as string, e.event_id as string]) assert(!log.includes(value), "a value was logged");
    assertEq(logged.length, 2);
  } finally {
    console.error = original;
  }
});
Deno.test("the database call carries the key only to the database", async () => {
  const seen: { url: string; headers: Headers; redirect?: RequestRedirect }[] = [];
  const fake = ((url: string, init: RequestInit) => {
    seen.push({ url, headers: new Headers(init.headers), redirect: init.redirect });
    return Promise.resolve(new Response('"accepted"'));
  }) as typeof fetch;
  assertEq(await restRpc("https://x.supabase.co", "sb_secret_abc", fake)("telemetry_submit", { p_row: {} }), "accepted");
  assertEq(seen[0].url, "https://x.supabase.co/rest/v1/rpc/telemetry_submit");
  assertEq(seen[0].headers.get("apikey"), "sb_secret_abc");
  assertEq(seen[0].headers.get("authorization"), null, "a secret key is not a JWT");
  assertEq(seen[0].redirect, "error");
  await restRpc("https://x.supabase.co", "eyJlegacy", fake)("telemetry_delete", {});
  assertEq(seen[1].headers.get("authorization"), "Bearer eyJlegacy");
});
Deno.test("a failed database call says nothing", async () => {
  const fake = (() => Promise.resolve(new Response("secret detail", { status: 500 }))) as unknown as typeof fetch;
  let message = "";
  try {
    await restRpc("https://x", "k", fake)("telemetry_submit", {});
  } catch (e) {
    message = (e as Error).message;
  }
  assertEq(message, "database call failed");
});
Deno.test("the service key comes from Supabase's secrets", () => {
  const env = (v: Record<string, string>) => (n: string) => v[n];
  assertEq(serviceKey(env({ SUPABASE_SECRET_KEYS: '{"default":"sb_secret_a"}', SUPABASE_SERVICE_ROLE_KEY: "old" })), "sb_secret_a");
  assertEq(serviceKey(env({ SUPABASE_SERVICE_ROLE_KEY: "old" })), "old");
  assertEq(serviceKey(env({ SUPABASE_SECRET_KEYS: "not json", SUPABASE_SERVICE_ROLE_KEY: "old" })), "old");
  assertEq(serviceKey(env({})), null);
});
// 20
Deno.test("retention removes what is older than 12 months and nothing else", async () => {
  const insertAt = async (when: string) => {
    const e = event(asInterrupted);
    assertEq(await submit(e), accepted);
    await db.query(`update public.telemetry_runs set received_at = ${when} where id = $1`, [e.event_id]);
    return e.event_id as string;
  };
  await clearLimits();
  const expired = await insertAt("now() - interval '12 months' - interval '1 minute'");
  const justInside = await insertAt("now() - interval '12 months' + interval '1 minute'");
  const recent = await insertAt("now()");
  await db.exec(`insert into public.telemetry_rate_limits values
    ('submit:global', now() - interval '3 days', 5), ('delete:global', date_trunc('hour', now(), 'UTC'), 5)`);
  const removed = await db.query<{ n: number }>("select public.telemetry_purge_expired() as n");
  assert(removed.rows[0].n >= 1, "nothing removed");
  assertEq(await count(db, "id = $1", [expired]), 0, "expired");
  assertEq(await count(db, "id = $1", [justInside]), 1, "one minute inside the period");
  assertEq(await count(db, "id = $1", [recent]), 1, "recent");
  const old = await db.query<{ c: number }>(
    "select count(*)::int as c from public.telemetry_rate_limits where window_start < date_trunc('day', now(), 'UTC')",
  );
  assertEq(old.rows[0].c, 0, "old counters");
  const kept = await db.query<{ c: number }>("select count(*)::int as c from public.telemetry_rate_limits where bucket = 'delete:global'");
  assertEq(kept.rows[0].c, 1, "a current counter stays");
  await clearLimits();
});
Deno.test("the purge takes no argument, so no caller can move the boundary", async () => {
  const r = await db.query<{ n: number }>("select pronargs as n from pg_proc where proname = 'telemetry_purge_expired'");
  assertEq(r.rows, [{ n: 0 }]);
});
Deno.test("the job is scheduled daily and runs only the purge", () => {
  const file = migrationFiles().find((n) => n.includes("schedule"));
  assert(file, "no schedule migration");
  const sql = readMigration(file);
  assert(sql.includes("cron.schedule('telemetry-retention', '17 3 * * *', 'select public.telemetry_purge_expired()')"), "the schedule changed");
  assertEq((sql.match(/cron\.schedule/g) ?? []).length, 1);
});
