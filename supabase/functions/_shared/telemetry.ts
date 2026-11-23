// The two public telemetry endpoints (docs/telemetry.md), apart from
// Deno.serve so the tests can call them. This file checks the shape of a
// request: every key, every type. Enumerations, ranges and the rules between
// fields are the database's constraints, so they exist in one place.
//
// Nothing here logs a request, a value or a database error.

export type Rpc = (name: string, args: Record<string, unknown>) => Promise<unknown>;

export const MAX_BODY_BYTES = 16 * 1024;
const MAX_AGE_MS = 30 * 24 * 60 * 60 * 1000;
const MAX_AHEAD_MS = 5 * 60 * 1000;

type Check = (v: unknown) => boolean;
type Obj = Record<string, unknown>;

const isObject = (v: unknown): v is Obj => typeof v === "object" && v !== null && !Array.isArray(v);
const text: Check = (v) => typeof v === "string" && v.length <= 128;
const int: Check = (v) => Number.isSafeInteger(v);
const num: Check = (v) => typeof v === "number" && Number.isFinite(v);
const bool: Check = (v) => typeof v === "boolean";
const uuid: Check = (v) =>
  typeof v === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(v);
const nullable = (c: Check): Check => (v) => v === null || c(v);
// Exactly these keys: an unknown field and a missing one are both refused. A
// value that was not measured is an explicit null.
const exact = (shape: Record<string, Check>): Check => (v) => {
  if (!isObject(v)) return false;
  const keys = Object.keys(v);
  return keys.length === Object.keys(shape).length &&
    keys.every((k) => Object.hasOwn(shape, k) && shape[k](v[k]));
};

const measurement = exact({
  verdict: text,
  seconds: nullable(num),
  score: nullable(num),
  power_w: nullable(int),
  core_mhz: nullable(int),
  memory_mhz: nullable(int),
  peak_temp_c: nullable(int),
  end_fan_pct: nullable(int),
  counted_seconds: nullable(num),
  counted_score: nullable(num),
  counted_power_w: nullable(int),
  counted_core_mhz: nullable(int),
  counted_memory_mhz: nullable(int),
  counted_temp_c: nullable(int),
  counted_fan_pct: nullable(int),
});

const eventShape = exact({
  schema_version: (v) => v === 1,
  event_id: uuid,
  installation_id: uuid,
  run_id: uuid,
  occurred_at: text,
  application: exact({ version: text, channel: text }),
  system: exact({ windows_version: nullable(text), windows_build: nullable(int) }),
  gpu: exact({
    name: nullable(text),
    architecture: nullable(text),
    vendor_id: nullable(int),
    device_id: nullable(int),
    subsystem_vendor_id: nullable(int),
    subsystem_id: nullable(int),
    vram_mb: nullable(int),
  }),
  driver: exact({ version: nullable(text) }),
  optimization: exact({
    type: text,
    profile: nullable(text),
    on_saved_tune: bool,
    duration_ms: nullable(int),
    outcome: text,
    failed_stage: nullable(text),
    error_code: nullable(text),
    driver_resets: nullable(int),
    stock_restored: nullable(bool),
    tune_saved: nullable(bool),
    measurement_disturbed: bool,
    steps: nullable(exact({ overclock: text, undervolt: text, fan_tune: text, final_test: text })),
  }),
  measurements: exact({ stock: nullable(measurement), result: nullable(measurement) }),
  tune: nullable(exact({
    core_offset_mhz: int,
    memory_offset_mhz: int,
    power_limit_pct: int,
    undervolt_enabled: bool,
    undervolt_mv: nullable(int),
    undervolt_mhz: nullable(int),
  })),
});

// "2026-11-23T18:30:00Z", a real date, from 30 days ago to 5 minutes ahead.
function timeInWindow(v: unknown, now: number): boolean {
  if (typeof v !== "string" || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/.test(v)) return false;
  const at = Date.parse(v);
  if (!Number.isFinite(at) || new Date(at).toISOString() !== v.replace("Z", ".000Z")) return false;
  return at >= now - MAX_AGE_MS && at <= now + MAX_AHEAD_MS;
}

export function validEvent(v: unknown, now: number): boolean {
  return eventShape(v) && timeInWindow((v as Obj).occurred_at, now);
}

// A validated event as the columns of telemetry_runs.
export function toRow(e: Obj): Obj {
  const app = e.application as Obj, sys = e.system as Obj, gpu = e.gpu as Obj, driver = e.driver as Obj;
  const opt = e.optimization as Obj, m = e.measurements as Obj;
  return {
    id: e.event_id,
    installation_id: e.installation_id,
    run_id: e.run_id,
    schema_version: e.schema_version,
    occurred_at: e.occurred_at,
    app_version: app.version,
    app_channel: app.channel,
    windows_version: sys.windows_version,
    windows_build: sys.windows_build,
    gpu_name: gpu.name,
    architecture: gpu.architecture,
    vendor_id: gpu.vendor_id,
    device_id: gpu.device_id,
    subsystem_vendor_id: gpu.subsystem_vendor_id,
    subsystem_id: gpu.subsystem_id,
    vram_mb: gpu.vram_mb,
    driver_version: driver.version,
    optimization_type: opt.type,
    profile: opt.profile,
    on_saved_tune: opt.on_saved_tune,
    duration_ms: opt.duration_ms,
    outcome: opt.outcome,
    failed_stage: opt.failed_stage,
    error_code: opt.error_code,
    driver_resets: opt.driver_resets,
    stock_restored: opt.stock_restored,
    tune_saved: opt.tune_saved,
    measurement_disturbed: opt.measurement_disturbed,
    stock_measurements: m.stock,
    result_measurements: m.result,
    tune: e.tune,
    steps: opt.steps,
  };
}

const reply = (status: number, body: Obj) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });

// The body as text, reading no more than the limit, whatever Content-Length says.
async function readBody(req: Request): Promise<{ text?: string; tooLarge?: boolean }> {
  if (Number(req.headers.get("content-length") ?? 0) > MAX_BODY_BYTES) {
    await req.body?.cancel();
    return { tooLarge: true };
  }
  if (!req.body) return { text: "" };
  const reader = req.body.getReader();
  const chunks: Uint8Array[] = [];
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    size += value.length;
    if (size > MAX_BODY_BYTES) {
      await reader.cancel();
      return { tooLarge: true };
    }
    chunks.push(value);
  }
  const all = new Uint8Array(size);
  let at = 0;
  for (const c of chunks) {
    all.set(c, at);
    at += c.length;
  }
  try {
    return { text: new TextDecoder("utf-8", { fatal: true }).decode(all) };
  } catch {
    return { text: "" };   // not UTF-8: fails as JSON below
  }
}

// The parsed JSON body; a Response when the request ends here.
async function jsonBody(req: Request, refuse: (status: number, error: string) => Response): Promise<{ value: unknown } | Response> {
  if (req.method !== "POST") return refuse(405, "METHOD_NOT_ALLOWED");
  const type = (req.headers.get("content-type") ?? "").split(";")[0].trim().toLowerCase();
  if (type !== "application/json") {
    await req.body?.cancel();
    return refuse(400, "INVALID_REQUEST");
  }
  const body = await readBody(req);
  if (body.tooLarge) return refuse(413, "TOO_LARGE");
  try {
    return { value: JSON.parse(body.text ?? "") };
  } catch {
    return refuse(400, "INVALID_REQUEST");
  }
}

export async function handleSubmit(req: Request, rpc: Rpc, now = Date.now()): Promise<Response> {
  const refuse = (status: number, error: string) =>
    reply(status, { accepted: false, error: error === "INVALID_REQUEST" ? "INVALID_EVENT" : error });
  const parsed = await jsonBody(req, refuse);
  if (parsed instanceof Response) return parsed;
  if (!validEvent(parsed.value, now)) return refuse(400, "INVALID_EVENT");
  let answer: unknown;
  try {
    answer = await rpc("telemetry_submit", { p_row: toRow(parsed.value as Obj) });
  } catch {
    console.error("submit-telemetry: database call failed");
    return refuse(500, "UNAVAILABLE");
  }
  switch (answer) {
    case "accepted": return reply(200, { accepted: true, duplicate: false });
    case "duplicate": return reply(200, { accepted: true, duplicate: true });
    case "rate_limited": return refuse(429, "RATE_LIMITED");
    case "invalid": return refuse(400, "INVALID_EVENT");
    default:
      console.error("submit-telemetry: unexpected database answer");
      return refuse(500, "UNAVAILABLE");
  }
}

export async function handleDelete(req: Request, rpc: Rpc): Promise<Response> {
  const refuse = (status: number, error: string) => reply(status, { deleted: false, error });
  const parsed = await jsonBody(req, refuse);
  if (parsed instanceof Response) return parsed;
  if (!exact({ installation_id: uuid })(parsed.value)) return refuse(400, "INVALID_REQUEST");
  let answer: unknown;
  try {
    answer = await rpc("telemetry_delete", { p_installation: (parsed.value as Obj).installation_id });
  } catch {
    console.error("delete-telemetry: database call failed");
    return refuse(500, "UNAVAILABLE");
  }
  if (answer === "deleted") return reply(200, { deleted: true });
  if (answer === "rate_limited") return refuse(429, "RATE_LIMITED");
  console.error("delete-telemetry: unexpected database answer");
  return refuse(500, "UNAVAILABLE");
}

// The key is one of Supabase's injected secrets, never in the repository or a
// response. The new secret keys come first; the legacy service-role key is
// being retired.
export function serviceKey(env: (name: string) => string | undefined): string | null {
  try {
    const key = JSON.parse(env("SUPABASE_SECRET_KEYS") ?? "{}").default;
    if (typeof key === "string" && key) return key;
  } catch {
    // fall through to the legacy key
  }
  return env("SUPABASE_SERVICE_ROLE_KEY") || null;
}

// A database function through PostgREST: no client library for two calls.
export function restRpc(url: string, key: string, fetchFn: typeof fetch = fetch): Rpc {
  const headers: Record<string, string> = { "content-type": "application/json", apikey: key };
  if (!key.startsWith("sb_")) headers.authorization = `Bearer ${key}`;   // a legacy JWT key
  return async (name, args) => {
    const res = await fetchFn(`${url}/rest/v1/rpc/${name}`, {
      method: "POST",
      headers,
      body: JSON.stringify(args),
      redirect: "error",
    });
    if (!res.ok) {
      await res.body?.cancel();
      throw new Error("database call failed");   // no status text or body: they can hold values
    }
    return await res.json();
  };
}

export function rpcFromEnv(): Rpc {
  const url = Deno.env.get("SUPABASE_URL");
  const key = serviceKey((name) => Deno.env.get(name));
  if (!url || !key) return () => Promise.reject(new Error("not configured"));
  return restRpc(url, key);
}
