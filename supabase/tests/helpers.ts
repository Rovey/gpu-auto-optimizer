// A real Postgres 17 (PGlite, in process) set up as a Supabase project is: its
// roles and its default grants, then the migrations. No network, no project.
import { PGlite } from "npm:@electric-sql/pglite@0.3.14";
import type { Rpc } from "../functions/_shared/telemetry.ts";

const supabaseDir = new URL("../", import.meta.url);

export type Row = Record<string, unknown>;

// The example event of docs/telemetry.md: the contract and the tests stay one.
export function docExample(): Row {
  const doc = Deno.readTextFileSync(new URL("../docs/telemetry.md", supabaseDir));
  const block = doc.match(/```json\r?\n([\s\S]*?)```/);
  if (!block) throw new Error("no JSON example in docs/telemetry.md");
  return JSON.parse(block[1]);
}

// The migrations in order. pg_cron does not exist in PGlite: the file that
// schedules the job is checked as text by the retention test.
export function migrationFiles(): string[] {
  return [...Deno.readDirSync(new URL("migrations/", supabaseDir))]
    .map((f) => f.name)
    .filter((n) => n.endsWith(".sql"))
    .sort();
}
export const readMigration = (name: string) => Deno.readTextFileSync(new URL(`migrations/${name}`, supabaseDir));

export async function freshDb(): Promise<PGlite> {
  const db = new PGlite();
  await db.exec(`
    create role anon nologin; create role authenticated nologin; create role service_role nologin bypassrls;
    grant usage on schema public to anon, authenticated, service_role;
    alter default privileges in schema public grant all on tables to anon, authenticated, service_role;
    alter default privileges in schema public grant all on functions to anon, authenticated, service_role;
  `);
  for (const name of migrationFiles()) {
    if (!name.includes("schedule")) await db.exec(readMigration(name));
  }
  return db;
}

export async function asRole<T>(db: PGlite, role: string, f: () => Promise<T>): Promise<T> {
  await db.exec(`set role ${role}`);
  try {
    return await f();
  } finally {
    await db.exec("reset role");
  }
}

// What PostgREST does for the Edge Function: the call, as the service role.
export function serviceRpc(db: PGlite): Rpc {
  return (name, args) =>
    asRole(db, "service_role", async () => {
      if (name === "telemetry_submit") {
        const r = await db.query<{ r: string }>("select public.telemetry_submit($1::jsonb) as r", [JSON.stringify(args.p_row)]);
        return r.rows[0].r;
      }
      if (name === "telemetry_delete") {
        const r = await db.query<{ r: string }>("select public.telemetry_delete($1::uuid) as r", [args.p_installation]);
        return r.rows[0].r;
      }
      throw new Error(`unexpected rpc ${name}`);
    });
}

export async function count(db: PGlite, where = "true", params: unknown[] = []): Promise<number> {
  const r = await db.query<{ c: number }>(`select count(*)::int as c from public.telemetry_runs where ${where}`, params);
  return r.rows[0].c;
}

let next = 0;
export const newId = () => {
  next++;
  const tail = next.toString(16).padStart(12, "0");
  return `00000000-0000-4000-8000-${tail}`;
};

export function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}
export function assertEq(actual: unknown, expected: unknown, message = "") {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${message ? message + ": " : ""}expected ${e}, got ${a}`);
}
