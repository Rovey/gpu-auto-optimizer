// End-to-end check of the telemetry backend on a throwaway LOCAL Supabase stack.
//   node supabase/tests/stack/run.mjs
// Needs Docker, Node and network access for the images. It starts the stack from
// empty volumes, runs every check, and removes the stack again. It never links,
// deploys or talks to a hosted project, and refuses any API that is not local.
// Any failed check, or a check that could not run, ends with exit code 1.
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { execFile, execFileSync } from 'node:child_process';
import { createHmac, randomUUID } from 'node:crypto';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const CLI = 'supabase@3.3.0';
const PROJECT = 'gpu-auto-optimizer-telemetry';
// What the CLI above starts. A different image fails the run: docs/telemetry.md lists these.
const IMAGES = {
  db: 'public.ecr.aws/supabase/postgres:17.12.0.002',
  rest: 'public.ecr.aws/supabase/postgrest:v16.6',
  kong: 'public.ecr.aws/supabase/kong:2.8.1',
  edge_runtime: 'public.ecr.aws/supabase/edge-runtime:v1.79.3',
};
const EXCLUDED = 'gotrue,realtime,storage-api,imgproxy,mailpit,studio,postgres-meta,logflare,vector,supavisor';
const container = (name) => `supabase_${name}_${PROJECT}`;
const db = container('db');
const shell = process.platform === 'win32';

const cli = (...args) =>
  execFileSync('npx', ['--yes', CLI, ...args], { cwd: root, shell, encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] });
const psql = ['exec', '-i', db, 'psql', '-U', 'postgres', '-d', 'postgres', '-v', 'ON_ERROR_STOP=1', '-tA'];
const sql = (q) => execFileSync('docker', [...psql, '-c', q], { encoding: 'utf8' }).trim();
const sqlAsync = (q) => promisify(execFile)('docker', [...psql, '-c', q], { encoding: 'utf8' });
const number = (q) => Number(sql(q));

let pass = 0, fail = 0;
const check = (name, got, want) => {
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a === b) { pass++; console.log(`ok   ${name}`); } else { fail++; console.log(`FAIL ${name}: got ${a}, wanted ${b}`); }
};
const tally = (list) => list.reduce((t, s) => ({ ...t, [s]: (t[s] ?? 0) + 1 }), {});

const example = JSON.parse(readFileSync(`${root}docs/telemetry.md`, 'utf8').match(/```json\r?\n([\s\S]*?)```/)[1]);
const event = (change = () => {}) => {
  const e = structuredClone(example);
  e.event_id = randomUUID(); e.run_id = randomUUID(); e.installation_id = randomUUID();
  e.occurred_at = new Date().toISOString().replace(/\.\d+Z$/, 'Z');
  change(e);
  return e;
};
const rows = (where) => number(`select count(*) from public.telemetry_runs where ${where}`);
const counters = (where = 'true') => number(`select count(*) from public.telemetry_rate_limits where ${where}`);
const hits = (bucket, unit) => number(`select coalesce(sum(hits), 0) from public.telemetry_rate_limits
  where bucket = ${bucket} and window_start = date_trunc('${unit}', now(), 'UTC')`);
const idBucket = (action, id) => `public.telemetry_id_bucket('${action}', '${id}')`;
const clearCounters = () => sql('delete from public.telemetry_rate_limits');

let fn;
// A role's token, signed with the local stack's own secret, straight to PostgREST
// from inside the stack's network: Kong's key handling is not what is under test.
let rest;

function connect() {
  const status = JSON.parse(cli('status', '-o', 'json'));
  const api = status.API_URL;
  if (!/^http:\/\/(127\.0\.0\.1|localhost):/.test(api ?? '')) throw new Error(`not a local API URL: ${api}`);
  fn = async (name, body) => {
    const r = await fetch(`${api}/functions/v1/${name}`, {
      method: 'POST',
      // No apikey and no Authorization: what the Windows client will send.
      headers: { 'content-type': 'application/json', 'x-region': 'eu-central-1' },
      body: typeof body === 'string' ? body : JSON.stringify(body),
    });
    const text = await r.text();
    let json = text;
    try { json = JSON.parse(text); } catch { /* compared as text */ }
    return { status: r.status, body: json };
  };

  const env = execFileSync('docker', ['inspect', container('rest'), '--format', '{{range .Config.Env}}{{println .}}{{end}}'], { encoding: 'utf8' });
  const raw = env.split(/\r?\n/).find((l) => l.startsWith('PGRST_JWT_SECRET='))?.slice('PGRST_JWT_SECRET='.length);
  if (!raw) throw new Error('PostgREST has no JWT secret');
  const oct = raw.startsWith('{') ? JSON.parse(raw).keys.find((k) => k.kty === 'oct') : null;
  const secret = oct ? Buffer.from(oct.k, 'base64url') : raw;
  const b64 = (v) => Buffer.from(v).toString('base64url');
  const token = (role) => {
    const at = Math.floor(Date.now() / 1000);
    const head = b64(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
    const claims = b64(JSON.stringify({ role, iss: 'supabase-demo', iat: at - 10, exp: at + 3600 }));
    return `${head}.${claims}.${createHmac('sha256', secret).update(`${head}.${claims}`).digest('base64url')}`;
  };
  rest = (role, method, path, body) => {
    const args = ['exec', db, 'curl', '-s', '-X', method, '-w', '\n%{http_code}', '-H', `Authorization: Bearer ${token(role)}`,
      '-H', 'Content-Type: application/json', `http://${container('rest')}:3000/${path}`];
    if (body) args.push('-d', JSON.stringify(body));
    const out = execFileSync('docker', args, { encoding: 'utf8' });
    const cut = out.lastIndexOf('\n');
    let code = '';
    try { code = JSON.parse(out.slice(0, cut)).code ?? ''; } catch { /* no JSON body */ }
    return { status: Number(out.slice(cut + 1)), code };
  };
}

function checkMigrations() {
  for (const [name, image] of Object.entries(IMAGES)) {
    check(`image of ${name}`, execFileSync('docker', ['inspect', container(name), '--format', '{{.Config.Image}}'], { encoding: 'utf8' }).trim(), image);
  }
  const files = readdirSync(`${root}supabase/migrations`).filter((f) => f.endsWith('.sql')).sort().map((f) => f.split('_')[0]);
  check('every migration applied, in order, from empty volumes',
    sql('select string_agg(version, \' \' order by version) from supabase_migrations.schema_migrations'), files.join(' '));
  const problems = execFileSync('docker', [...psql], { input: readFileSync(`${root}supabase/tests/audit.sql`, 'utf8'), encoding: 'utf8' }).trim();
  check('catalog audit finds no problem', problems, '');
  for (const role of ['anon', 'authenticated', 'service_role']) {
    check(`${role} has no access to the cron schema`, sql(`select has_schema_privilege('${role}', 'cron', 'usage')`), 'f');
  }
  check('cron job scheduled', sql(`select schedule || ' | ' || command || ' | ' || active from cron.job where jobname = 'telemetry-retention'`),
    '17 3 * * * | select public.telemetry_purge_expired() | true');
  check('one scheduled job in total', number('select count(*) from cron.job'), 1);
}

async function checkEndpoints() {
  const e = event();
  check('submit: valid event, no key', await fn('submit-telemetry', e), { status: 200, body: { accepted: true, duplicate: false } });
  check('submit: same event', await fn('submit-telemetry', e), { status: 200, body: { accepted: true, duplicate: true } });
  check('submit: same run, other event', await fn('submit-telemetry', { ...e, event_id: randomUUID() }), { status: 200, body: { accepted: true, duplicate: true } });
  check('one row for the run', rows(`run_id = '${e.run_id}'`), 1);
  check('stored JSON keeps nulls', sql(`select stock_measurements -> 'counted_score' from public.telemetry_runs where id = '${e.event_id}'`), 'null');
  const invalid = { status: 400, body: { accepted: false, error: 'INVALID_EVENT' } };
  check('submit: aborted outcome', await fn('submit-telemetry', event((x) => { x.optimization.outcome = 'aborted'; })), invalid);
  check('submit: unknown field', await fn('submit-telemetry', event((x) => { x.gpu.uuid = 'GPU-1'; })), invalid);
  check('submit: not JSON', await fn('submit-telemetry', '{'), invalid);
  check('submit: too large', (await fn('submit-telemetry', JSON.stringify({ a: 'x'.repeat(17000) }))).status, 413);
  check('submit: old timestamp', await fn('submit-telemetry', event((x) => { x.occurred_at = '2026-01-02T00:00:00Z'; })), invalid);
  const interrupted = event((x) => {
    Object.assign(x.optimization, { outcome: 'interrupted', duration_ms: null, driver_resets: null, stock_restored: null, tune_saved: null, steps: null });
    x.measurements = { stock: null, result: null }; x.tune = null;
  });
  check('submit: interrupted', (await fn('submit-telemetry', interrupted)).status, 200);
  check('interrupted stored without results',
    sql(`select tune is null and result_measurements is null and stock_measurements is null from public.telemetry_runs where id = '${interrupted.event_id}'`), 't');

  const asFailed = (x, stock, code, more) => {
    Object.assign(x.optimization, {
      type: 'undervolt', profile: 'undervolt', outcome: 'failed', failed_stage: 'stock', error_code: code,
      additional_error_codes: more, tune_saved: false, stock_restored: stock, steps: null,
    });
    x.measurements.result = null; x.tune = null;
  };
  const several = event((x) => asFailed(x, false, 'STOCK_RESTORE_FAILED', ['INTERNAL_ERROR', 'TOO_HOT', 'SAVE_FAILED']));
  check('submit: simultaneous failures', (await fn('submit-telemetry', several)).status, 200);
  check('all causes stored, in order', sql(`select error_code || ' ' || array_to_string(additional_error_codes, ' ') from public.telemetry_runs where id = '${several.event_id}'`),
    'STOCK_RESTORE_FAILED INTERNAL_ERROR TOO_HOT SAVE_FAILED');
  check('submit: an internal error hiding a failed reset', await fn('submit-telemetry', event((x) => asFailed(x, false, 'INTERNAL_ERROR', []))), invalid);
  check('submit: free text as a code', await fn('submit-telemetry', event((x) => asFailed(x, true, 'TOO_HOT', ['C:/Users/x']))), invalid);

  // What a role can reach. 42501 is the database refusing: a JWT error (PGRST30x) would prove nothing.
  const uuid = e.installation_id;
  const denied = [
    ['read runs', 'GET', 'telemetry_runs?select=*'],
    ['insert run', 'POST', 'telemetry_runs', { id: randomUUID() }],
    ['update run', 'PATCH', `telemetry_runs?id=eq.${e.event_id}`, { gpu_name: 'x' }],
    ['delete run', 'DELETE', `telemetry_runs?id=eq.${e.event_id}`],
    ['read counters', 'GET', 'telemetry_rate_limits?select=*'],
    ['change counters', 'PATCH', 'telemetry_rate_limits?hits=gt.0', { hits: 0 }],
    ['delete counters', 'DELETE', 'telemetry_rate_limits?hits=gt.0'],
    ['rpc purge', 'POST', 'rpc/telemetry_purge_expired', {}],
    ['rpc take', 'POST', 'rpc/telemetry_take', { p_bucket: 'submit:global', p_window: new Date().toISOString(), p_limit: 1 }],
    ['rpc admit', 'POST', 'rpc/telemetry_admit', { p_action: 'submit', p_installation: uuid, p_per_day: 60, p_per_hour: 1000 }],
    ['rpc bucket', 'POST', 'rpc/telemetry_id_bucket', { p_action: 'submit', p_installation: uuid }],
    ['rpc validator', 'POST', 'rpc/telemetry_valid_tune', { t: {} }],
  ];
  for (const role of ['anon', 'authenticated', 'service_role']) {
    for (const [what, method, path, body] of denied) {
      const r = rest(role, method, path, body);
      check(`${role} denied by the database: ${what} (${r.status} ${r.code})`, r.code, '42501');
    }
  }
  for (const role of ['anon', 'authenticated']) {
    check(`${role} denied: rpc submit`, rest(role, 'POST', 'rpc/telemetry_submit', { p_row: {} }).code, '42501');
    check(`${role} denied: rpc delete`, rest(role, 'POST', 'rpc/telemetry_delete', { p_installation: uuid }).code, '42501');
  }
  // The control: the same token path does reach the two entry points.
  check('control: service_role calls telemetry_submit', rest('service_role', 'POST', 'rpc/telemetry_submit', { p_row: {} }).status, 200);
  check('control: service_role calls telemetry_delete', rest('service_role', 'POST', 'rpc/telemetry_delete', { p_installation: randomUUID() }).status, 200);
  check('row survived every attempt', rows(`id = '${e.event_id}'`), 1);

  const known = await fn('delete-telemetry', { installation_id: e.installation_id });
  const unknown = await fn('delete-telemetry', { installation_id: randomUUID() });
  check('delete: known', known, { status: 200, body: { deleted: true } });
  check('delete: unknown answers the same', unknown, known);
  check('delete removed the installation', rows(`installation_id = '${e.installation_id}'`), 0);
  check('delete left the other', rows(`id = '${interrupted.event_id}'`), 1);
  check('delete: bad body', (await fn('delete-telemetry', { installation_id: '*' })).status, 400);
}

async function checkConcurrency() {
  const send = (e) => fn('submit-telemetry', e);
  const remove = (id) => fn('delete-telemetry', { installation_id: id });
  const inBatches = async (items, size, f) => {
    const out = [];
    for (let i = 0; i < items.length; i += size) out.push(...await Promise.all(items.slice(i, i + size).map(f)));
    return out;
  };
  const hour = `date_trunc('hour', now(), 'UTC')`, today = `date_trunc('day', now(), 'UTC')`;

  clearCounters();
  const mine = randomUUID();
  let r = await inBatches(Array.from({ length: 90 }, () => event((x) => { x.installation_id = mine; })), 15, send);
  check('90 events of one installation: answers', tally(r.map((x) => x.status)), { 200: 60, 429: 30 });
  check('... rows', rows(`installation_id = '${mine}'`), 60);
  check('... its counter', hits(idBucket('submit', mine), 'day'), 60);
  check('... the global counter, nothing lost in a race', hits(`'submit:global'`, 'hour'), 60);

  clearCounters();
  const same = event();
  r = await Promise.all(Array.from({ length: 10 }, () => send(same)));
  check('one event 10 times at once: new', r.filter((x) => x.body.accepted && !x.body.duplicate).length, 1);
  check('... duplicates', r.filter((x) => x.body.accepted && x.body.duplicate).length, 9);
  check('... rows', rows(`run_id = '${same.run_id}'`), 1);
  check('... a duplicate takes no hit: installation', hits(idBucket('submit', same.installation_id), 'day'), 1);
  check('... a duplicate takes no hit: global', hits(`'submit:global'`, 'hour'), 1);

  clearCounters();
  sql(`insert into public.telemetry_rate_limits values ('submit:global', ${hour}, 990)`);
  r = await Promise.all(Array.from({ length: 30 }, () => send(event())));
  check('the last 10 global hits, 30 installations at once', tally(r.map((x) => x.status)), { 200: 10, 429: 20 });
  check('... the global counter stops at its limit', hits(`'submit:global'`, 'hour'), 1000);
  check('... a counter only for those let in', counters(`bucket <> 'submit:global'`), 10);

  r = await Promise.all(Array.from({ length: 30 }, () => send(event())));
  check('rotating IDs on a full hour: all refused', tally(r.map((x) => x.status)), { 429: 30 });
  check('... and no new counter', counters(`bucket <> 'submit:global'`), 10);

  clearCounters();
  const del = randomUUID();
  r = await Promise.all(Array.from({ length: 30 }, () => remove(del)));
  check('30 deletions of one installation at once', tally(r.map((x) => x.status)), { 200: 10, 429: 20 });
  check('... its counter', hits(idBucket('delete', del), 'day'), 10);
  check('... the global counter', hits(`'delete:global'`, 'hour'), 10);

  // Cleanup while requests and the purge run: no deadlock, no active counter lost.
  clearCounters();
  sql(`insert into public.telemetry_rate_limits
       select 'submit:' || lpad(to_hex(i), 64, '0'), ${today} - interval '1 day', 1 from generate_series(1, 300) as i`);
  sql(`insert into public.telemetry_rate_limits
       select 'submit:' || lpad(to_hex(i), 64, 'a'), ${today}, 7 from generate_series(1, 20) as i`);
  const load = await Promise.all([
    ...Array.from({ length: 25 }, () => send(event())),
    ...Array.from({ length: 25 }, () => remove(randomUUID())),
    sqlAsync('select public.telemetry_purge_expired()').then(() => ({ status: 200 })),
    sqlAsync('select public.telemetry_purge_expired()').then(() => ({ status: 200 })),
  ]);
  check('cleanup under load: every request answered 200', tally(load.map((x) => x.status)), { 200: 52 });
  check('... no ended counter left', counters(`window_start < ${today}`), 0);
  check('... active counters kept', counters(`bucket like 'submit:aaaa%' and window_start = ${today} and hits = 7`), 20);
  clearCounters();
}

async function checkRetention() {
  const old = event(); await fn('submit-telemetry', old);
  const inside = event(); await fn('submit-telemetry', inside);
  sql(`update public.telemetry_runs set received_at = now() - interval '12 months' - interval '1 minute' where id = '${old.event_id}'`);
  sql(`update public.telemetry_runs set received_at = now() - interval '12 months' + interval '10 minutes' where id = '${inside.event_id}'`);
  sql(`insert into public.telemetry_rate_limits values
       ('submit:${'b'.repeat(64)}', date_trunc('day', now(), 'UTC') - interval '1 day', 1),
       ('submit:${'c'.repeat(64)}', date_trunc('day', now(), 'UTC'), 1)`);
  // The real scheduler, once: a copy of the job, every minute.
  sql(`select cron.schedule('telemetry-retention-test', '* * * * *', 'select public.telemetry_purge_expired()')`);
  let ran = '';
  for (let i = 0; i < 40 && !ran; i++) {
    await new Promise((done) => setTimeout(done, 3000));
    ran = sql(`select d.status from cron.job_run_details d join cron.job j using (jobid)
               where j.jobname = 'telemetry-retention-test' and d.status in ('succeeded', 'failed') limit 1`);
  }
  sql(`select cron.unschedule('telemetry-retention-test')`);
  check('scheduler ran the purge', ran, 'succeeded');
  check('scheduler removed the expired row', rows(`id = '${old.event_id}'`), 0);
  check('scheduler kept the row just inside', rows(`id = '${inside.event_id}'`), 1);
  check('scheduler removed the ended counter', counters(`bucket = 'submit:${'b'.repeat(64)}'`), 0);
  check('scheduler kept the active counter', counters(`bucket = 'submit:${'c'.repeat(64)}'`), 1);
}

if (existsSync(`${root}supabase/.temp/project-ref`)) throw new Error('this checkout is linked to a hosted project: refusing to run');
let started = false;
try {
  cli('stop', '--no-backup');
  started = true;
  cli('start', '-x', EXCLUDED);
  connect();
  checkMigrations();
  await checkEndpoints();
  await checkConcurrency();
  await checkRetention();
} catch (e) {
  fail++;
  console.log(`FAIL the run stopped: ${e.message}`);
} finally {
  if (started) {
    try { cli('stop', '--no-backup'); } catch { console.log('note: the stack could not be stopped'); }
  }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail || !pass ? 1 : 0);
