// Checks a deployed telemetry backend and prints the evidence, with synthetic
// events only. The procedure is in docs/telemetry-dev-verification.md.
//
//   node supabase/tests/remote/verify.mjs --project-ref <ref> --confirm-not-production <ref>
//
// Needs SUPABASE_ACCESS_TOKEN in the environment. It refuses a project whose
// telemetry table holds any row, and it removes what it created. No token, key or
// full project ref is printed. Exit code: 0 passed, 1 failed, 2 refused to start.
//
// `--local --api <url>` is the rehearsal on the local stack (supabase/tests/stack/run.mjs).
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const REGION = 'eu-central-1';
const LOCAL_DB = 'supabase_db_gpu-auto-optimizer-telemetry';
const refuse = (why) => { console.error(`refused: ${why}`); process.exit(2); };

const { values: opt } = parseArgs({
  options: {
    'project-ref': { type: 'string' },
    'confirm-not-production': { type: 'string' },
    local: { type: 'boolean', default: false },
    api: { type: 'string' },
  },
});

let api, query, exec, publicKey, dbRegion = null, projectStatus = null, shownRef;

if (opt.local) {
  api = opt.api;
  if (!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(api ?? '')) refuse('--local needs a local --api URL');
  publicKey = process.env.VERIFY_PUBLIC_KEY;
  if (!publicKey) refuse('--local needs VERIFY_PUBLIC_KEY');
  const psql = (input) => execFileSync('docker', ['exec', '-i', LOCAL_DB, 'psql', '-U', 'postgres', '-d', 'postgres', '-v', 'ON_ERROR_STOP=1', '-tA'],
    { input, encoding: 'utf8' });
  query = async (sql) => JSON.parse(psql(`select coalesce(json_agg(t), '[]') from (${sql}\n) t;`).trim());
  exec = async (sql) => { psql(`${sql};`); };
  shownRef = 'local stack';
} else {
  const ref = opt['project-ref'];
  const token = process.env.SUPABASE_ACCESS_TOKEN;
  if (!/^[a-z0-9]{20}$/.test(ref ?? '')) refuse('--project-ref must be a project ref');
  if (opt['confirm-not-production'] !== ref) refuse('--confirm-not-production must repeat the ref of a development project');
  if (!token) refuse('SUPABASE_ACCESS_TOKEN is not set');
  api = `https://${ref}.supabase.co`;
  shownRef = `${ref.slice(0, 4)}...`;
  const mgmt = async (path, init = {}) => {
    const r = await fetch(`https://api.supabase.com/v1/projects/${ref}${path}`, {
      ...init,
      redirect: 'error',
      headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json' },
    });
    const text = await r.text();
    if (!r.ok) throw new Error(`Management API ${path.split('?')[0] || '/'}: HTTP ${r.status}`);
    return text ? JSON.parse(text) : null;
  };
  query = (sql) => mgmt('/database/query', { method: 'POST', body: JSON.stringify({ query: sql }) });
  exec = async (sql) => { await query(sql); };
  try {
    const project = await mgmt('');
    dbRegion = project.region;
    projectStatus = project.status;
    const keys = await mgmt('/api-keys?reveal=false');
    publicKey = keys.find((k) => k.type === 'publishable')?.api_key ?? keys.find((k) => k.name === 'anon')?.api_key;
  } catch (e) {
    refuse(e.message);
  }
  if (projectStatus !== 'ACTIVE_HEALTHY') refuse(`project status is ${projectStatus}`);
  if (!publicKey) refuse('the project has no publishable or anon key to test with');
}

let pass = 0, fail = 0;
const check = (name, got, want) => {
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a === b) { pass++; console.log(`ok   ${name}`); } else { fail++; console.log(`FAIL ${name}: got ${a}, wanted ${b}`); }
};

const example = JSON.parse(readFileSync(`${root}docs/telemetry.md`, 'utf8').match(/```json\r?\n([\s\S]*?)```/)[1]);
const installations = [];
const event = (change = () => {}) => {
  const e = structuredClone(example);
  e.event_id = randomUUID(); e.run_id = randomUUID(); e.installation_id = randomUUID();
  e.occurred_at = new Date().toISOString().replace(/\.\d+Z$/, 'Z');
  change(e);
  installations.push(e.installation_id);
  return e;
};
// No apikey and no Authorization: what the Windows client will send.
const call = async (name, body, method = 'POST') => {
  const r = await fetch(`${api}/functions/v1/${name}`, {
    method,
    redirect: 'error',
    headers: { 'content-type': 'application/json', 'x-region': REGION },
    body: method === 'POST' ? (typeof body === 'string' ? body : JSON.stringify(body)) : undefined,
  });
  const text = await r.text();
  let json = text;
  try { json = JSON.parse(text); } catch { /* compared as text */ }
  return { status: r.status, body: json, region: r.headers.get('x-sb-edge-region') };
};
const answer = ({ status, body }) => ({ status, body });
const rows = async (where) => Number((await query(`select count(*) as c from public.telemetry_runs where ${where}`))[0].c);
const invalid = { status: 400, body: { accepted: false, error: 'INVALID_EVENT' } };

const evidence = { checked_at: new Date().toISOString(), target: shownRef };
let preflightPassed = false;

async function run() {
  // Before anything is written: this must be an empty, throwaway backend.
  const existing = Number((await query('select count(*) as c from public.telemetry_runs'))[0].c);
  if (existing !== 0) {
    console.error(`refused: telemetry_runs holds ${existing} rows, so this is not an empty development project`);
    process.exitCode = 2;
    return;
  }
  preflightPassed = true;

  // Two separate facts: where the database is, and where a function really ran.
  if (!opt.local) check(`database region is ${REGION}`, dbRegion, REGION);
  evidence.database_region = opt.local ? 'not applicable (local)' : dbRegion;
  evidence.database_region_source = opt.local ? null : 'Management API, GET /v1/projects/{ref}';
  const probes = [];
  for (let i = 0; i < 10; i++) probes.push(await call('submit-telemetry', {}));
  check('10 probes without a key are refused as invalid', probes.map(answer), probes.map(() => invalid));
  const seen = [...new Set(probes.map((p) => p.region))];
  evidence.edge_regions_seen = opt.local ? 'not applicable (local)' : seen;
  evidence.edge_region_source = opt.local ? null : `x-sb-edge-region on 10 responses, sent with x-region: ${REGION}`;
  if (!opt.local) check(`every function response ran in ${REGION}`, seen, [REGION]);

  const e = event();
  check('valid event accepted', answer(await call('submit-telemetry', e)), { status: 200, body: { accepted: true, duplicate: false } });
  check('same event is a duplicate', answer(await call('submit-telemetry', e)), { status: 200, body: { accepted: true, duplicate: true } });
  check('one row', await rows(`run_id = '${e.run_id}'`), 1);
  check('aborted refused as an outcome', answer(await call('submit-telemetry', event((x) => { x.optimization.outcome = 'aborted'; }))), invalid);
  check('unknown field refused', answer(await call('submit-telemetry', event((x) => { x.gpu.uuid = 'GPU-1'; }))), invalid);
  check('free text as a code refused', answer(await call('submit-telemetry', event((x) => { x.optimization.additional_error_codes = ['C:/Users/x']; }))), invalid);
  check('NUL in a text refused', answer(await call('submit-telemetry', event((x) => { x.gpu.name = 'NVIDIA\u0000'; }))), invalid);
  check('malformed JSON refused', answer(await call('submit-telemetry', '{')), invalid);
  check('oversized body refused', (await call('submit-telemetry', JSON.stringify({ a: 'x'.repeat(17000) }))).status, 413);
  check('GET refused', (await call('submit-telemetry', null, 'GET')).status, 405);

  // The public key must not reach the data. 42501 is the database refusing.
  const denied = [
    ['read runs', 'GET', 'telemetry_runs?select=id&limit=1'],
    ['read counters', 'GET', 'telemetry_rate_limits?select=hits&limit=1'],
    ['call submit', 'POST', 'rpc/telemetry_submit', { p_row: {} }],
    ['call delete', 'POST', 'rpc/telemetry_delete', { p_installation: e.installation_id }],
    ['call purge', 'POST', 'rpc/telemetry_purge_expired', {}],
  ];
  for (const [what, method, path, body] of denied) {
    const r = await fetch(`${api}/rest/v1/${path}`, {
      method,
      redirect: 'error',
      // A publishable key is not a JWT and does not belong in Authorization.
      headers: { apikey: publicKey, ...(publicKey.startsWith('eyJ') ? { authorization: `Bearer ${publicKey}` } : {}), 'content-type': 'application/json' },
      body: body ? JSON.stringify(body) : undefined,
    });
    let code = '';
    try { code = JSON.parse(await r.text()).code ?? ''; } catch { /* no JSON body */ }
    check(`public key denied by the database: ${what} (${r.status})`, code, '42501');
  }
  check('row survived', await rows(`id = '${e.event_id}'`), 1);

  const audit = await query(readFileSync(`${root}supabase/tests/audit.sql`, 'utf8'));
  check('catalog audit finds no problem', audit.map((p) => p.problem), []);
  const job = await query(`select schedule, command, active from cron.job where jobname = 'telemetry-retention'`);
  check('retention job is scheduled', job, [{ schedule: '17 3 * * *', command: 'select public.telemetry_purge_expired()', active: true }]);
  const last = await query(`select d.status, d.start_time from cron.job_run_details d join cron.job j using (jobid)
                            where j.jobname = 'telemetry-retention' order by d.start_time desc limit 1`);
  evidence.retention_job_last_run = last[0] ?? 'has not run yet: check again after 03:17 UTC';

  const other = event();
  check('second installation stored', (await call('submit-telemetry', other)).status, 200);
  const known = await call('delete-telemetry', { installation_id: e.installation_id });
  const unknown = await call('delete-telemetry', { installation_id: randomUUID() });
  check('deletion answered', answer(known), { status: 200, body: { deleted: true } });
  check('unknown ID gets the same answer', answer(unknown), answer(known));
  check('deletion removed the installation', await rows(`installation_id = '${e.installation_id}'`), 0);
  check('deletion left the other installation', await rows(`id = '${other.event_id}'`), 1);

  const inside = event();
  check('third installation stored', (await call('submit-telemetry', inside)).status, 200);
  await exec(`update public.telemetry_runs set received_at = now() - interval '12 months' - interval '1 minute' where id = '${other.event_id}'`);
  await exec(`update public.telemetry_runs set received_at = now() - interval '12 months' + interval '10 minutes' where id = '${inside.event_id}'`);
  await exec('select public.telemetry_purge_expired()');
  check('purge removed the expired row', await rows(`id = '${other.event_id}'`), 0);
  check('purge kept the row just inside 12 months', await rows(`id = '${inside.event_id}'`), 1);
}

try {
  await run();
} catch (e) {
  fail++;
  console.log(`FAIL the run stopped: ${e.message}`);
} finally {
  if (preflightPassed) {
    try {
      // The table was empty when the run began, so this removes only its own rows.
      await exec(`delete from public.telemetry_runs where installation_id in (${installations.map((i) => `'${i}'`).join(',')})`);
      await exec('delete from public.telemetry_rate_limits');
      check('nothing left behind', Number((await query('select count(*) as c from public.telemetry_runs'))[0].c), 0);
    } catch (e) {
      fail++;
      console.log(`FAIL cleanup: ${e.message}`);
    }
  }
}
if (preflightPassed) {
  console.log(`\nevidence: ${JSON.stringify(evidence, null, 2)}`);
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exitCode = fail || !pass ? 1 : 0;
}
