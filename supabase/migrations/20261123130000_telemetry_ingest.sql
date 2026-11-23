-- What the Edge Functions call: one insert, deleting one installation, and the
-- retention cleanup, with the rate limits. See docs/telemetry.md.
--
-- The service role can execute telemetry_submit and telemetry_delete and has no
-- other right here. Those two are SECURITY DEFINER for that reason, so a leaked
-- service key cannot read the data or get past the limits. They take typed
-- arguments, run no dynamic SQL, name every object with its schema, and search
-- pg_catalog before pg_temp (see the first migration).

-- Counters per fixed time window. An installation appears only as a SHA-256 of
-- its ID. A counter is removed once its window has ended.
create table public.telemetry_rate_limits (
  bucket       text not null check (bucket ~ '^(submit|delete):(global|[0-9a-f]{64})$'),
  window_start timestamptz not null,
  hits         integer not null check (hits >= 0),   -- 0: its only hit was given back
  primary key (bucket, window_start)
);
create index telemetry_rate_limits_window_idx on public.telemetry_rate_limits (window_start);

alter table public.telemetry_rate_limits enable row level security;
revoke all on table public.telemetry_rate_limits from public, anon, authenticated, service_role;

-- Takes one hit from a bucket; false when its window is full. One statement,
-- so concurrent requests cannot both take the last hit.
create function public.telemetry_take(p_bucket text, p_window timestamptz, p_limit integer)
returns boolean
language plpgsql
set search_path = pg_catalog, pg_temp
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
set search_path = pg_catalog, pg_temp
as $$
  select p_action || ':' || encode(sha256(convert_to(p_installation::text, 'UTF8')), 'hex')
$$;

-- Admits one request, or answers false and writes nothing.
--
-- The global hit comes before the installation's counter, so a new counter row
-- exists only for a request the global limit let in: rotating installation IDs
-- cannot add rows once the hour is full. An installation already at its limit
-- is refused first, so it cannot use up the global limit.
--
-- Every admitted request removes up to 10 counters of windows that have ended
-- and adds at most one, so the table drains without the scheduled job. Windows
-- are at most a UTC day long and aligned to it: one that began before today is
-- over, and no active counter can be removed. SKIP LOCKED: two requests cleaning
-- up at once neither wait for each other nor deadlock.
create function public.telemetry_admit(p_action text, p_installation uuid, p_per_day integer, p_per_hour integer)
returns boolean
language plpgsql
set search_path = pg_catalog, pg_temp
as $$
declare
  v_mine text := public.telemetry_id_bucket(p_action, p_installation);
  v_all text := p_action || ':global';
  v_day timestamptz := date_trunc('day', now(), 'UTC');
  v_hour timestamptz := date_trunc('hour', now(), 'UTC');
begin
  if exists (select 1 from public.telemetry_rate_limits
             where bucket = v_mine and window_start = v_day and hits >= p_per_day) then
    return false;
  end if;
  if not public.telemetry_take(v_all, v_hour, p_per_hour) then
    return false;
  end if;
  if not public.telemetry_take(v_mine, v_day, p_per_day) then
    -- Lost a race for the installation's last hit: the global hit goes back.
    update public.telemetry_rate_limits set hits = hits - 1 where bucket = v_all and window_start = v_hour;
    return false;
  end if;
  delete from public.telemetry_rate_limits
  where ctid in (select ctid from public.telemetry_rate_limits
                 where window_start < v_day
                 order by window_start
                 limit 10
                 for update skip locked);
  return true;
end;
$$;

-- p_row: the flat row the Edge Function built from a validated event. Answers
-- 'accepted', 'duplicate', 'rate_limited' or 'invalid'. A duplicate takes no
-- hit, so a client can always clear its outbox. received_at is the server's.
create function public.telemetry_submit(p_row jsonb)
returns text
language plpgsql
security definer
set search_path = pg_catalog, pg_temp
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

  if not public.telemetry_admit('submit', v_installation, 60, 1000) then
    return 'rate_limited';
  end if;

  -- Its own block: an event the constraints refuse keeps the hits it took.
  begin
    insert into public.telemetry_runs (
      id, installation_id, run_id, schema_version, occurred_at, app_version, app_channel, windows_version, windows_build,
      gpu_name, architecture, vendor_id, device_id, subsystem_vendor_id, subsystem_id, vram_mb, driver_version,
      optimization_type, profile, on_saved_tune, duration_ms, outcome, failed_stage, error_code, additional_error_codes,
      driver_resets, stock_restored, tune_saved, measurement_disturbed, stock_measurements, result_measurements, tune, steps)
    select
      r.id, r.installation_id, r.run_id, r.schema_version, r.occurred_at, r.app_version, r.app_channel, r.windows_version, r.windows_build,
      r.gpu_name, r.architecture, r.vendor_id, r.device_id, r.subsystem_vendor_id, r.subsystem_id, r.vram_mb, r.driver_version,
      r.optimization_type, r.profile, r.on_saved_tune, r.duration_ms, r.outcome, r.failed_stage, r.error_code, r.additional_error_codes,
      r.driver_resets, r.stock_restored, r.tune_saved, r.measurement_disturbed, r.stock_measurements, r.result_measurements, r.tune, r.steps
    from jsonb_populate_record(null::public.telemetry_runs, p_row) as r
    on conflict do nothing;
    if not found then
      -- The same event, a moment earlier: its hits go back.
      update public.telemetry_rate_limits set hits = hits - 1
      where (bucket = 'submit:global' and window_start = date_trunc('hour', now(), 'UTC'))
         or (bucket = public.telemetry_id_bucket('submit', v_installation) and window_start = date_trunc('day', now(), 'UTC'));
      return 'duplicate';
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
security definer
set search_path = pg_catalog, pg_temp
as $$
begin
  if p_installation is null then
    return 'invalid';
  end if;
  if not public.telemetry_admit('delete', p_installation, 10, 600) then
    return 'rate_limited';
  end if;
  delete from public.telemetry_runs where installation_id = p_installation;
  return 'deleted';
end;
$$;

-- Removes runs received more than 12 months ago, and every counter whose window
-- has ended. It takes no argument, so no caller can move either boundary.
-- Returns the runs removed.
create function public.telemetry_purge_expired()
returns integer
language plpgsql
set search_path = pg_catalog, pg_temp
as $$
declare
  v_removed integer;
begin
  delete from public.telemetry_runs where received_at < now() - interval '12 months';
  get diagnostics v_removed = row_count;
  delete from public.telemetry_rate_limits where window_start < date_trunc('day', now(), 'UTC');
  return v_removed;
end;
$$;

revoke all on function public.telemetry_take(text, timestamptz, integer) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_id_bucket(text, uuid) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_admit(text, uuid, integer, integer) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_submit(jsonb) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_delete(uuid) from public, anon, authenticated, service_role;
-- Only the owner, which is the role the scheduled job runs as.
revoke all on function public.telemetry_purge_expired() from public, anon, authenticated, service_role;

grant execute on function public.telemetry_submit(jsonb) to service_role;
grant execute on function public.telemetry_delete(uuid) to service_role;
