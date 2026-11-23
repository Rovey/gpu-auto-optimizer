-- What the Edge Functions call: rate limits, one insert, deleting one
-- installation, and the retention cleanup. See docs/telemetry.md.

-- Counters per fixed time window. An installation appears only as a SHA-256 of
-- its ID, and a counter is removed about two days after its window began.
create table public.telemetry_rate_limits (
  bucket       text not null check (bucket ~ '^(submit|delete):(global|[0-9a-f]{64})$'),
  window_start timestamptz not null,
  hits         integer not null check (hits > 0),
  primary key (bucket, window_start)
);
create index telemetry_rate_limits_window_idx on public.telemetry_rate_limits (window_start);

alter table public.telemetry_rate_limits enable row level security;
revoke all on table public.telemetry_rate_limits from public, anon, authenticated, service_role;
grant select, insert, update on table public.telemetry_rate_limits to service_role;

-- Takes one hit from a bucket; false when its window is full. One statement,
-- so concurrent requests cannot both take the last hit.
create function public.telemetry_take(p_bucket text, p_window timestamptz, p_limit integer)
returns boolean
language plpgsql
set search_path = ''
as $$
begin
  insert into public.telemetry_rate_limits as r (bucket, window_start, hits)
  values (p_bucket, p_window, 1)
  on conflict (bucket, window_start) do update set hits = r.hits + 1 where r.hits < p_limit;
  return found;
end;
$$;

create function public.telemetry_id_bucket(p_action text, p_installation uuid)
returns text
language sql
immutable
set search_path = ''
as $$
  select p_action || ':' || encode(sha256(convert_to(p_installation::text, 'UTF8')), 'hex')
$$;

-- p_row: the flat row the Edge Function built from a validated event. Answers
-- 'accepted', 'duplicate', 'rate_limited' or 'invalid'. A duplicate takes no
-- hit, so a client can always clear its outbox. received_at is the server's.
create function public.telemetry_submit(p_row jsonb)
returns text
language plpgsql
set search_path = ''
as $$
declare
  v_id uuid;
  v_run uuid;
  v_installation uuid;
begin
  begin
    v_id := (p_row ->> 'id')::uuid;
    v_run := (p_row ->> 'run_id')::uuid;
    v_installation := (p_row ->> 'installation_id')::uuid;
  exception when data_exception then
    return 'invalid';
  end;
  if v_id is null or v_run is null or v_installation is null then
    return 'invalid';
  end if;

  if exists (select 1 from public.telemetry_runs where id = v_id or run_id = v_run) then
    return 'duplicate';
  end if;

  if not public.telemetry_take(public.telemetry_id_bucket('submit', v_installation), date_trunc('day', now(), 'UTC'), 60) then
    return 'rate_limited';
  end if;
  if not public.telemetry_take('submit:global', date_trunc('hour', now(), 'UTC'), 1000) then
    return 'rate_limited';
  end if;

  -- Its own block: an event the constraints refuse keeps the hits it took.
  begin
    insert into public.telemetry_runs (
      id, installation_id, run_id, schema_version, occurred_at, app_version, app_channel, windows_version, windows_build,
      gpu_name, architecture, vendor_id, device_id, subsystem_vendor_id, subsystem_id, vram_mb, driver_version,
      optimization_type, profile, on_saved_tune, duration_ms, outcome, failed_stage, error_code, driver_resets,
      stock_restored, tune_saved, measurement_disturbed, stock_measurements, result_measurements, tune, steps)
    select
      r.id, r.installation_id, r.run_id, r.schema_version, r.occurred_at, r.app_version, r.app_channel, r.windows_version, r.windows_build,
      r.gpu_name, r.architecture, r.vendor_id, r.device_id, r.subsystem_vendor_id, r.subsystem_id, r.vram_mb, r.driver_version,
      r.optimization_type, r.profile, r.on_saved_tune, r.duration_ms, r.outcome, r.failed_stage, r.error_code, r.driver_resets,
      r.stock_restored, r.tune_saved, r.measurement_disturbed, r.stock_measurements, r.result_measurements, r.tune, r.steps
    from jsonb_populate_record(null::public.telemetry_runs, p_row) as r
    on conflict do nothing;
    if not found then
      return 'duplicate';   -- the same event, a moment earlier
    end if;
    return 'accepted';
  exception when integrity_constraint_violation or data_exception then
    return 'invalid';
  end;
end;
$$;

-- Deletes every record of one installation. Answers 'deleted' whether or not
-- there were any, or 'rate_limited'. Hits do not depend on that either.
create function public.telemetry_delete(p_installation uuid)
returns text
language plpgsql
set search_path = ''
as $$
begin
  if p_installation is null then
    return 'invalid';
  end if;
  if not public.telemetry_take(public.telemetry_id_bucket('delete', p_installation), date_trunc('day', now(), 'UTC'), 10) then
    return 'rate_limited';
  end if;
  if not public.telemetry_take('delete:global', date_trunc('hour', now(), 'UTC'), 600) then
    return 'rate_limited';
  end if;
  delete from public.telemetry_runs where installation_id = p_installation;
  return 'deleted';
end;
$$;

-- Removes runs received more than 12 months ago, and old counters. It takes no
-- argument, so no caller can move the boundary. Returns the runs removed.
create function public.telemetry_purge_expired()
returns integer
language plpgsql
set search_path = ''
as $$
declare
  v_removed integer;
begin
  delete from public.telemetry_runs where received_at < now() - interval '12 months';
  get diagnostics v_removed = row_count;
  delete from public.telemetry_rate_limits where window_start < now() - interval '2 days';
  return v_removed;
end;
$$;

revoke all on function public.telemetry_take(text, timestamptz, integer) from public, anon, authenticated;
revoke all on function public.telemetry_id_bucket(text, uuid) from public, anon, authenticated;
revoke all on function public.telemetry_submit(jsonb) from public, anon, authenticated;
revoke all on function public.telemetry_delete(uuid) from public, anon, authenticated;
-- Only the owner, which is the role the scheduled job runs as.
revoke all on function public.telemetry_purge_expired() from public, anon, authenticated, service_role;

grant execute on function public.telemetry_take(text, timestamptz, integer) to service_role;
grant execute on function public.telemetry_id_bucket(text, uuid) to service_role;
grant execute on function public.telemetry_submit(jsonb) to service_role;
grant execute on function public.telemetry_delete(uuid) to service_role;
