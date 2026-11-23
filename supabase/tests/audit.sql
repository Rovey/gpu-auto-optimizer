-- The access model of schema `public`, read from the catalog. One row per
-- problem: no rows means it holds. Run by the Deno tests and the stack check.
with rels as (
  select c.relname, c.relkind, c.relowner, c.relrowsecurity, c.relforcerowsecurity, c.relacl
  from pg_class c join pg_namespace n on n.oid = c.relnamespace
  where n.nspname = 'public' and c.relkind in ('r', 'p', 'v', 'm', 'f', 'S')
),
funcs as (
  select p.oid, p.proname, p.proowner, p.prosecdef, p.proconfig, p.proacl,
         p.proname in ('telemetry_submit', 'telemetry_delete') as is_entry
  from pg_proc p join pg_namespace n on n.oid = p.pronamespace
  where n.nspname = 'public'
)
select 'not a plain table: ' || relname as problem from rels where relkind <> 'r'
union all
select 'no row level security: ' || relname from rels where not relrowsecurity
union all
-- FORCE would lock out the owner, and with it the two entry points.
select 'forced row level security: ' || relname from rels where relforcerowsecurity
union all
select 'a policy on ' || tablename || ': ' || policyname from pg_policies where schemaname = 'public'
union all
select format('%s on %s for %s', a.privilege_type, r.relname, a.grantee::regrole)
from rels r, aclexplode(coalesce(r.relacl, acldefault('r', r.relowner))) a
where a.grantee <> r.relowner
union all
select 'search_path not empty: ' || proname from funcs where not coalesce(proconfig = array['search_path=""'], false)
union all
select 'SECURITY DEFINER: ' || proname from funcs where prosecdef and not is_entry
union all
select 'not SECURITY DEFINER: ' || proname from funcs where is_entry and not prosecdef
union all
select format('%s on function %s for %s', a.privilege_type, f.proname, a.grantee::regrole)
from funcs f, aclexplode(coalesce(f.proacl, acldefault('f', f.proowner))) a
where a.grantee <> f.proowner
  and not (f.is_entry and a.grantee = 'service_role'::regrole and a.privilege_type = 'EXECUTE' and not a.is_grantable)
union all
select 'service_role cannot execute ' || proname from funcs
where is_entry and not has_function_privilege('service_role', oid, 'execute')
union all
select 'entry point missing: ' || e.name
from unnest(array['telemetry_submit', 'telemetry_delete', 'telemetry_purge_expired']) as e(name)
where not exists (select 1 from funcs f where f.proname = e.name)
union all
select 'table missing: ' || e.name
from unnest(array['telemetry_runs', 'telemetry_rate_limits']) as e(name)
where not exists (select 1 from rels r where r.relname = e.name)
