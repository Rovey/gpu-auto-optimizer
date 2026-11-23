-- One row per top-level optimization run. The contract is docs/telemetry.md.
-- Clients never reach this table: the Edge Functions call telemetry_submit and
-- telemetry_delete with the service role, which has no right on the table itself.
--
-- The validators use CASE because SQL does not promise the order of AND: a
-- cast or a key lookup must not run on a value of the wrong JSON type. A CHECK
-- passes on NULL, so every test of a key that must be there reads as false
-- when it is missing.
--
-- Every function sets search_path to pg_catalog, pg_temp. An empty search_path is
-- not enough: pg_temp is then searched FIRST for types, so a caller's temporary
-- domain named `uuid` or `text` would run its CHECK with the owner's rights.

-- A JSON value that is absent, null, or a number in [lo, hi].
create function public.telemetry_number_in(v jsonb, lo numeric, hi numeric)
returns boolean
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case
    when v is null or jsonb_typeof(v) = 'null' then true
    when jsonb_typeof(v) = 'number' then (v #>> '{}')::numeric between lo and hi
    else false
  end
$$;

-- The same, for a whole number.
create function public.telemetry_int_in(v jsonb, lo numeric, hi numeric)
returns boolean
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case
    when v is null or jsonb_typeof(v) = 'null' then true
    when jsonb_typeof(v) = 'number' then
      (v #>> '{}')::numeric between lo and hi and (v #>> '{}')::numeric = trunc((v #>> '{}')::numeric)
    else false
  end
$$;

-- One measurement (StabilityResult): null, or an object of known keys in range.
-- A reading the card did not give is null, never 0. `verdict` is the verdict of
-- the measurement, where `aborted` is a real value; it is not a run outcome.
create function public.telemetry_valid_measurement(m jsonb)
returns boolean
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case
    when m is null then true
    when jsonb_typeof(m) <> 'object' then false
    when exists (
      select 1 from jsonb_object_keys(m) as k
      where k not in ('verdict', 'seconds', 'score', 'power_w', 'core_mhz', 'memory_mhz', 'peak_temp_c', 'end_fan_pct',
                      'counted_seconds', 'counted_score', 'counted_power_w', 'counted_core_mhz', 'counted_memory_mhz',
                      'counted_temp_c', 'counted_fan_pct')
    ) then false
    when not coalesce(jsonb_typeof(m -> 'verdict') = 'string'
                      and m ->> 'verdict' in ('stable', 'wrong_result', 'device_lost', 'too_hot', 'no_telemetry', 'aborted', 'stalled'),
                      false) then false
    when not (public.telemetry_number_in(m -> 'seconds', 0, 86400)
              and public.telemetry_number_in(m -> 'score', 0.001, 10000000)
              and public.telemetry_int_in(m -> 'power_w', 0, 2000)
              and public.telemetry_int_in(m -> 'core_mhz', 0, 6000)
              and public.telemetry_int_in(m -> 'memory_mhz', 0, 50000)
              and public.telemetry_int_in(m -> 'peak_temp_c', 0, 130)
              and public.telemetry_int_in(m -> 'end_fan_pct', 0, 150)
              and public.telemetry_number_in(m -> 'counted_seconds', 0, 86400)
              and public.telemetry_number_in(m -> 'counted_score', 0.001, 10000000)
              and public.telemetry_int_in(m -> 'counted_power_w', 0, 2000)
              and public.telemetry_int_in(m -> 'counted_core_mhz', 0, 6000)
              and public.telemetry_int_in(m -> 'counted_memory_mhz', 0, 50000)
              and public.telemetry_int_in(m -> 'counted_temp_c', 0, 130)
              and public.telemetry_int_in(m -> 'counted_fan_pct', 0, 150)) then false
    -- StabilityResult starts out as Stable: a measurement that never ran is null, not "stable".
    when m ->> 'verdict' = 'stable' and not coalesce((m ->> 'seconds')::numeric > 0, false) then false
    -- The counted part exists only for a run that covered its full length.
    when coalesce((m ->> 'counted_seconds')::numeric, 0) > 0 then m ->> 'verdict' = 'stable'
    else m ->> 'counted_score' is null and m ->> 'counted_power_w' is null and m ->> 'counted_core_mhz' is null
         and m ->> 'counted_memory_mhz' is null and m ->> 'counted_temp_c' is null and m ->> 'counted_fan_pct' is null
  end
$$;

-- The tune a run saved: null, or a whole tune. With an undervolt it has both of
-- the undervolt's values, without one neither.
create function public.telemetry_valid_tune(t jsonb)
returns boolean
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case
    when t is null then true
    when jsonb_typeof(t) <> 'object' then false
    when exists (
      select 1 from jsonb_object_keys(t) as k
      where k not in ('core_offset_mhz', 'memory_offset_mhz', 'power_limit_pct', 'undervolt_enabled', 'undervolt_mv', 'undervolt_mhz')
    ) then false
    when not coalesce(jsonb_typeof(t -> 'core_offset_mhz') = 'number' and jsonb_typeof(t -> 'memory_offset_mhz') = 'number'
                      and jsonb_typeof(t -> 'power_limit_pct') = 'number' and jsonb_typeof(t -> 'undervolt_enabled') = 'boolean',
                      false) then false
    when not (public.telemetry_int_in(t -> 'core_offset_mhz', -2000, 2000)
              and public.telemetry_int_in(t -> 'memory_offset_mhz', -6000, 6000)
              and public.telemetry_int_in(t -> 'power_limit_pct', 1, 250)
              and public.telemetry_int_in(t -> 'undervolt_mv', 400, 1500)
              and public.telemetry_int_in(t -> 'undervolt_mhz', 100, 6000)) then false
    when t ->> 'undervolt_enabled' = 'true' then
      coalesce(jsonb_typeof(t -> 'undervolt_mv') = 'number' and jsonb_typeof(t -> 'undervolt_mhz') = 'number', false)
    else t ->> 'undervolt_mv' is null and t ->> 'undervolt_mhz' is null
  end
$$;

-- What became of the four steps of an All in one run: null for any other run.
create function public.telemetry_valid_steps(s jsonb)
returns boolean
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case
    when s is null then true
    when jsonb_typeof(s) <> 'object' then false
    else (
      select count(*) = 4
             and coalesce(bool_and(e.key in ('overclock', 'undervolt', 'fan_tune', 'final_test')
                                   and jsonb_typeof(e.value) = 'string'
                                   and e.value #>> '{}' in ('ok', 'failed', 'not_reached')), false)
      from jsonb_each(s) as e
    )
  end
$$;

-- The error codes, most important first. A run can fail in several ways at
-- once: `error_code` is the first that applies, and `additional_error_codes`
-- holds the others in this same order, so one set of causes has one spelling.
-- What leaves the card in a wrong state comes first and cannot be hidden.
create function public.telemetry_error_codes()
returns text[]
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select array['GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED', 'INTERNAL_ERROR', 'TOO_HOT', 'DRIVER_RESET',
               'STABILITY_CHECK_FAILED', 'MEASUREMENT_FAILED', 'FAN_CONTROL_FAILED', 'UNCLASSIFIED',
               'NOT_KEPT', 'SAVE_FAILED']::text[]
$$;

-- Every code of a run, the first one included.
create function public.telemetry_all_errors(p_code text, p_more text[])
returns text[]
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case when p_code is null then p_more else array_prepend(p_code, p_more) end
$$;

-- The additional codes: known, without repeats, in order, all after the first
-- code. Without a first code there are none.
create function public.telemetry_valid_more_errors(p_code text, p_more text[])
returns boolean
language sql
immutable
set search_path = pg_catalog, pg_temp
as $$
  select case
    when p_more is null then false
    when cardinality(p_more) = 0 then true
    when p_code is null then false
    else coalesce(
      array_position(o.codes, p_code) < array_position(o.codes, p_more[1])
      and p_more = (select array_agg(k.c order by array_position(o.codes, k.c))
                    from (select distinct u.c from unnest(p_more) as u(c) where u.c = any(o.codes)) as k(c)),
      false)
  end
  from (select public.telemetry_error_codes() as codes) as o
$$;

create table public.telemetry_runs (
  id                  uuid primary key,                 -- event_id: a retry cannot create a second row
  installation_id     uuid not null,
  run_id              uuid not null unique,             -- one row per run, also under another event_id
  schema_version      smallint not null check (schema_version = 1),
  occurred_at         timestamptz not null check (occurred_at >= '2026-01-01'),
  received_at         timestamptz not null default now(),

  app_version         text not null check (app_version ~ '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}(-(rc|check|test|dev)[0-9]{1,4})?$'),
  app_channel         text not null check (app_channel in ('stable', 'prerelease')),
  windows_version     text check (windows_version ~ '^Windows (10|11|Server 20[0-9]{2})$'),
  windows_build       integer check (windows_build between 1 and 1000000),

  -- As the driver names the card: a product family, then the model. No character
  -- a path or an address needs.
  gpu_name            text check (gpu_name ~ '^(NVIDIA|GeForce|Quadro|TITAN|Tesla) [A-Za-z0-9 ._+()-]{1,56}$'),
  architecture        text check (architecture in ('Pascal', 'Volta', 'Turing', 'Ampere', 'Ada', 'Blackwell')),
  vendor_id           integer check (vendor_id = 4318),  -- 0x10DE, NVIDIA
  device_id           integer check (device_id between 0 and 65535),
  subsystem_vendor_id integer check (subsystem_vendor_id between 0 and 65535),
  subsystem_id        integer check (subsystem_id between 0 and 65535),
  vram_mb             integer check (vram_mb between 1 and 1048576),
  driver_version      text check (driver_version ~ '^[0-9]{1,4}\.[0-9]{1,4}$'),

  optimization_type   text not null check (optimization_type in
                        ('best_of_my_gpu', 'quiet', 'cool_and_efficient', 'max_performance', 'undervolt', 'all_in_one',
                         'saved_tune_retest')),
  profile             text check (profile in
                        ('best_of_my_gpu', 'quiet', 'cool_and_efficient', 'max_performance', 'undervolt', 'all_in_one')),
  on_saved_tune       boolean not null default false,   -- its "before" is the saved tune, not stock
  duration_ms         bigint check (duration_ms between 0 and 86400000),
  outcome             text not null check (outcome in ('success', 'failed', 'no_result', 'cancelled', 'interrupted')),
  failed_stage        text check (failed_stage in ('stock', 'search', 'soak', 'undervolt', 'fan_tune', 'final_test', 'retest')),
  error_code          text check (error_code = any (public.telemetry_error_codes())),
  additional_error_codes text[] not null default '{}',  -- the other causes of this run, never free text
  driver_resets       integer check (driver_resets between 0 and 100),
  stock_restored      boolean,                          -- null: the run did not have to, or it is not known
  tune_saved          boolean,                          -- null: not known (interrupted)
  measurement_disturbed boolean not null default false, -- another program changed its use of the card during the run

  stock_measurements  jsonb check (public.telemetry_valid_measurement(stock_measurements)),
  result_measurements jsonb check (public.telemetry_valid_measurement(result_measurements)),
  tune                jsonb check (public.telemetry_valid_tune(tune)),
  steps               jsonb check (public.telemetry_valid_steps(steps)),

  constraint telemetry_error_set check (public.telemetry_valid_more_errors(error_code, additional_error_codes)),
  constraint telemetry_channel_fits_version check ((app_version like '%-%') = (app_channel = 'prerelease')),

  -- What each outcome may say.
  constraint telemetry_success_is_clean check (
    outcome <> 'success' or (tune_saved is true and tune is not null and failed_stage is null and error_code is null
                             and stock_restored is null
                             and coalesce(result_measurements ->> 'verdict' = 'stable', false))
  ),
  constraint telemetry_failed_has_code check (
    outcome <> 'failed' or (error_code is not null and error_code not in ('NOT_KEPT', 'SAVE_FAILED')
                            and not 'NOT_KEPT' = any (additional_error_codes))
  ),
  -- A result that was not kept is a reason, not a technical failure.
  constraint telemetry_no_result_has_reason check (
    outcome <> 'no_result' or (
      public.telemetry_all_errors(error_code, additional_error_codes)
        <@ array['GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED', 'NOT_KEPT', 'SAVE_FAILED']::text[]
      and public.telemetry_all_errors(error_code, additional_error_codes) && array['NOT_KEPT', 'SAVE_FAILED']::text[]
    )
  ),
  constraint telemetry_cancelled_blames_nothing check (
    outcome <> 'cancelled' or (failed_stage is null
                               and public.telemetry_all_errors(error_code, additional_error_codes)
                                   <@ array['INTERNAL_ERROR', 'GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED', 'SAVE_FAILED']::text[])
  ),
  constraint telemetry_interrupted_claims_nothing check (
    outcome <> 'interrupted' or (duration_ms is null and driver_resets is null and stock_restored is null and tune_saved is null
                                 and stock_measurements is null and result_measurements is null and tune is null and steps is null
                                 and failed_stage is null and error_code is null and measurement_disturbed is false)
  ),
  constraint telemetry_finished_run_is_known check (
    outcome = 'interrupted' or (duration_ms is not null and driver_resets is not null and tune_saved is not null)
  ),
  -- A retest saves each lower step it falls back to, so it can end failed or
  -- stopped with a tune saved. No other run keeps anything unless it succeeded.
  constraint telemetry_saved_means_success check (
    tune_saved is not true or outcome = 'success' or (optimization_type = 'saved_tune_retest' and outcome in ('failed', 'cancelled'))
  ),
  constraint telemetry_tune_only_when_saved check (tune is null or tune_saved is true),
  -- A failed reset to stock is always the first code, whatever else went wrong.
  -- GPU_UNREACHABLE is that same failure after a driver reset: one or the other.
  constraint telemetry_stock_failure_has_code check (
    (stock_restored is not false or coalesce(error_code in ('GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED'), false))
    and (error_code is null or error_code not in ('GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED') or stock_restored is false)
    and not additional_error_codes && array['GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED']::text[]
  ),
  -- A write that failed kept nothing of this run as its result.
  constraint telemetry_save_failure_saved_nothing check (
    not 'SAVE_FAILED' = any (public.telemetry_all_errors(error_code, additional_error_codes)) or tune_saved is false
  ),
  -- "None of the known causes" says nothing next to a known cause.
  constraint telemetry_unclassified_is_alone check (
    not 'UNCLASSIFIED' = any (additional_error_codes) or error_code in ('GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED')
  ),

  -- What each kind of run can have.
  constraint telemetry_profile_fits_type check (
    optimization_type = 'saved_tune_retest' or on_saved_tune or profile is not distinct from optimization_type
  ),
  constraint telemetry_saved_tune_has_no_stock check (not on_saved_tune or (optimization_type = 'undervolt' and stock_measurements is null)),
  constraint telemetry_retest_has_no_stock check (optimization_type <> 'saved_tune_retest' or stock_measurements is null),
  constraint telemetry_steps_only_all_in_one check (steps is null or optimization_type = 'all_in_one'),
  constraint telemetry_all_in_one_has_steps check (optimization_type <> 'all_in_one' or outcome = 'interrupted' or steps is not null),
  constraint telemetry_not_kept_not_retest check (
    not 'NOT_KEPT' = any (public.telemetry_all_errors(error_code, additional_error_codes)) or optimization_type <> 'saved_tune_retest'
  ),
  constraint telemetry_fan_failure_in_fan_step check (
    not 'FAN_CONTROL_FAILED' = any (public.telemetry_all_errors(error_code, additional_error_codes))
    or (optimization_type = 'all_in_one' and coalesce(failed_stage in ('fan_tune', 'final_test'), false))
  ),
  constraint telemetry_stage_fits_type check (
    failed_stage is null or (
      (failed_stage not in ('fan_tune', 'final_test') or optimization_type = 'all_in_one')
      and (failed_stage = 'retest') = (optimization_type = 'saved_tune_retest')
    )
  )
);

create index telemetry_runs_card_idx on public.telemetry_runs (device_id, subsystem_id);
create index telemetry_runs_gpu_name_idx on public.telemetry_runs (gpu_name);
create index telemetry_runs_driver_idx on public.telemetry_runs (driver_version);
create index telemetry_runs_app_version_idx on public.telemetry_runs (app_version);
create index telemetry_runs_outcome_idx on public.telemetry_runs (outcome);
create index telemetry_runs_received_idx on public.telemetry_runs (received_at);   -- retention
create index telemetry_runs_installation_idx on public.telemetry_runs (installation_id);   -- deletion

-- No policies: with RLS on, a role without BYPASSRLS sees and writes nothing.
-- The explicit revokes are needed too: Supabase's default privileges grant new
-- objects in `public` to anon, authenticated and service_role, TRUNCATE and
-- UPDATE included. No role but the owner has any right on this table: the
-- service role reaches it only through telemetry_submit and telemetry_delete.
alter table public.telemetry_runs enable row level security;
revoke all on table public.telemetry_runs from public, anon, authenticated, service_role;

revoke all on function public.telemetry_number_in(jsonb, numeric, numeric) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_int_in(jsonb, numeric, numeric) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_valid_measurement(jsonb) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_valid_tune(jsonb) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_valid_steps(jsonb) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_error_codes() from public, anon, authenticated, service_role;
revoke all on function public.telemetry_all_errors(text, text[]) from public, anon, authenticated, service_role;
revoke all on function public.telemetry_valid_more_errors(text, text[]) from public, anon, authenticated, service_role;
