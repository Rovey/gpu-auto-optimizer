-- One row per top-level optimization run. The contract is docs/telemetry.md.
-- Clients never reach this table: the submit-telemetry Edge Function is the
-- only writer, with the service role.

-- A JSON value that is absent, null, or a number in [lo, hi].
create function public.telemetry_number_in(v jsonb, lo numeric, hi numeric)
returns boolean
language sql
immutable
set search_path = ''
as $$
  select v is null
      or jsonb_typeof(v) = 'null'
      or (jsonb_typeof(v) = 'number' and (v #>> '{}')::numeric between lo and hi)
$$;

-- One measurement (StabilityResult): null, or an object of known keys in range.
-- A reading the card did not give is null, never 0.
create function public.telemetry_valid_measurement(m jsonb)
returns boolean
language sql
immutable
set search_path = ''
as $$
  select m is null or (
    jsonb_typeof(m) = 'object'
    and not exists (
      select 1 from jsonb_object_keys(m) as k
      where k not in ('verdict', 'seconds', 'score', 'power_w', 'core_mhz', 'memory_mhz', 'peak_temp_c', 'end_fan_pct',
                      'counted_seconds', 'counted_score', 'counted_power_w', 'counted_core_mhz', 'counted_memory_mhz',
                      'counted_temp_c', 'counted_fan_pct')
    )
    -- coalesce: a CHECK passes on NULL, so a missing key must read as false
    and coalesce(m ->> 'verdict' in ('stable', 'wrong_result', 'device_lost', 'too_hot', 'no_telemetry', 'aborted', 'stalled'), false)
    and public.telemetry_number_in(m -> 'seconds', 0, 86400)
    and public.telemetry_number_in(m -> 'score', 0.001, 10000000)
    and public.telemetry_number_in(m -> 'power_w', 0, 2000)
    and public.telemetry_number_in(m -> 'core_mhz', 0, 6000)
    and public.telemetry_number_in(m -> 'memory_mhz', 0, 50000)
    and public.telemetry_number_in(m -> 'peak_temp_c', 0, 130)
    and public.telemetry_number_in(m -> 'end_fan_pct', 0, 150)
    and public.telemetry_number_in(m -> 'counted_seconds', 0, 86400)
    and public.telemetry_number_in(m -> 'counted_score', 0.001, 10000000)
    and public.telemetry_number_in(m -> 'counted_power_w', 0, 2000)
    and public.telemetry_number_in(m -> 'counted_core_mhz', 0, 6000)
    and public.telemetry_number_in(m -> 'counted_memory_mhz', 0, 50000)
    and public.telemetry_number_in(m -> 'counted_temp_c', 0, 130)
    and public.telemetry_number_in(m -> 'counted_fan_pct', 0, 150)
  )
$$;

-- The tune a run saved: null, or an object of known keys in range.
create function public.telemetry_valid_tune(t jsonb)
returns boolean
language sql
immutable
set search_path = ''
as $$
  select t is null or (
    jsonb_typeof(t) = 'object'
    and not exists (
      select 1 from jsonb_object_keys(t) as k
      where k not in ('core_offset_mhz', 'memory_offset_mhz', 'power_limit_pct', 'undervolt_enabled', 'undervolt_mv', 'undervolt_mhz')
    )
    and public.telemetry_number_in(t -> 'core_offset_mhz', -2000, 2000)
    and public.telemetry_number_in(t -> 'memory_offset_mhz', -6000, 6000)
    and public.telemetry_number_in(t -> 'power_limit_pct', 1, 250)
    and coalesce(jsonb_typeof(t -> 'undervolt_enabled') = 'boolean', false)
    and public.telemetry_number_in(t -> 'undervolt_mv', 400, 1500)
    and public.telemetry_number_in(t -> 'undervolt_mhz', 100, 6000)
  )
$$;

-- What became of the steps of an All in one run: null for any other run.
create function public.telemetry_valid_steps(s jsonb)
returns boolean
language sql
immutable
set search_path = ''
as $$
  select s is null or (
    jsonb_typeof(s) = 'object'
    and not exists (
      select 1 from jsonb_each_text(s) as e
      where e.key not in ('overclock', 'undervolt', 'fan_tune', 'final_test')
         or e.value is null
         or e.value not in ('ok', 'failed', 'not_reached')
    )
  )
$$;

create table public.telemetry_runs (
  id                  uuid primary key,                 -- event_id: a retry cannot create a second row
  installation_id     uuid not null,
  run_id              uuid not null unique,             -- one event per run
  schema_version      smallint not null check (schema_version = 1),
  occurred_at         timestamptz not null check (occurred_at >= '2026-01-01'),
  received_at         timestamptz not null default now(),

  app_version         text not null check (app_version ~ '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}(-[A-Za-z0-9.]{1,24})?$'),
  app_channel         text not null check (app_channel in ('stable', 'prerelease')),
  windows_version     text check (windows_version ~ '^[A-Za-z0-9 .]{1,32}$'),
  windows_build       integer check (windows_build between 1 and 1000000),

  gpu_name            text check (gpu_name ~ '^[ -~]{1,128}$'),
  architecture        text check (architecture ~ '^[A-Za-z0-9 _-]{1,32}$'),
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
  outcome             text not null check (outcome in ('success', 'failed', 'no_result', 'cancelled', 'aborted', 'interrupted')),
  failed_stage        text check (failed_stage in ('stock', 'search', 'soak', 'undervolt', 'fan_tune', 'final_test', 'retest')),
  error_code          text check (error_code in
                        ('STABILITY_CHECK_FAILED', 'DRIVER_RESET', 'GPU_UNREACHABLE', 'STOCK_RESTORE_FAILED', 'FAN_CONTROL_FAILED',
                         'MEASUREMENT_FAILED', 'TOO_HOT', 'NOT_KEPT', 'INTERNAL_ERROR', 'UNCLASSIFIED')),
  driver_resets       integer check (driver_resets between 0 and 100),
  stock_restored      boolean,                          -- null: the run did not have to, or it is not known
  tune_saved          boolean,                          -- null: not known (interrupted)
  measurement_disturbed boolean not null default false, -- another program changed its use of the card during the run

  stock_measurements  jsonb check (public.telemetry_valid_measurement(stock_measurements)),
  result_measurements jsonb check (public.telemetry_valid_measurement(result_measurements)),
  tune                jsonb check (public.telemetry_valid_tune(tune)),
  steps               jsonb check (public.telemetry_valid_steps(steps)),

  constraint telemetry_success_is_clean check (
    outcome <> 'success' or (tune_saved is true and failed_stage is null and error_code is null)
  ),
  constraint telemetry_failed_has_code check (outcome <> 'failed' or error_code is not null),
  constraint telemetry_interrupted_claims_nothing check (
    outcome <> 'interrupted' or (duration_ms is null and driver_resets is null and stock_restored is null and tune_saved is null
                                 and result_measurements is null and tune is null)
  ),
  constraint telemetry_tune_only_when_saved check (tune is null or tune_saved is true),
  constraint telemetry_steps_only_all_in_one check (steps is null or optimization_type = 'all_in_one')
);

create index telemetry_runs_card_idx on public.telemetry_runs (device_id, subsystem_id);
create index telemetry_runs_gpu_name_idx on public.telemetry_runs (gpu_name);
create index telemetry_runs_driver_idx on public.telemetry_runs (driver_version);
create index telemetry_runs_app_version_idx on public.telemetry_runs (app_version);
create index telemetry_runs_outcome_idx on public.telemetry_runs (outcome);
create index telemetry_runs_received_idx on public.telemetry_runs (received_at);
-- The rate limit per installation, and deleting an installation's records.
create index telemetry_runs_installation_idx on public.telemetry_runs (installation_id, received_at);

-- No policies: with RLS on, a role without BYPASSRLS sees and writes nothing.
-- The explicit revokes are needed too, because Supabase's default privileges
-- grant new objects in `public` to anon and authenticated.
alter table public.telemetry_runs enable row level security;
revoke all on table public.telemetry_runs from public, anon, authenticated;
grant select, insert, delete on table public.telemetry_runs to service_role;

revoke all on function public.telemetry_number_in(jsonb, numeric, numeric) from public, anon, authenticated;
revoke all on function public.telemetry_valid_measurement(jsonb) from public, anon, authenticated;
revoke all on function public.telemetry_valid_tune(jsonb) from public, anon, authenticated;
revoke all on function public.telemetry_valid_steps(jsonb) from public, anon, authenticated;
grant execute on function public.telemetry_number_in(jsonb, numeric, numeric) to service_role;
grant execute on function public.telemetry_valid_measurement(jsonb) to service_role;
grant execute on function public.telemetry_valid_tune(jsonb) to service_role;
grant execute on function public.telemetry_valid_steps(jsonb) to service_role;
