-- Once a day, 03:17 UTC. Scheduling under the same name replaces the job.
create extension if not exists pg_cron;
select cron.schedule('telemetry-retention', '17 3 * * *', 'select public.telemetry_purge_expired()');
