# Telemetry: verifying a Supabase development project

**Status: not run.** On 2026-11-23 no development project or access token was available, so nothing has been deployed and
every row of the evidence table below is empty. The design and the API contract are in `docs/telemetry.md`.

## Rules

- **Development only.** Never run this against a project that holds real data. The tool refuses a table with rows, but it
  cannot tell what a project is for: that is the owner's call.
- **No cost.** Use an organization on the Free plan. If a screen shows a monthly price, stop. In a paid organization each
  project adds compute cost.
- **No secrets in the repository, logs or chat.** The access token and the database password are typed into your own
  terminal. The tool prints neither, and shows only the first four characters of the project ref.
- **Configuration is not evidence.** The database region and the region a function ran in are separate facts, and each has
  its own check.
- The Windows application stays off. It has no client for this backend yet.

## 1. Create the project (owner)

1. In the Supabase dashboard, choose an organization on the **Free** plan and create a new project named
   `gao-telemetry-dev`.
2. Region: **Central EU (Frankfurt)**, which is `eu-central-1`.
3. Keep the generated database password in a password manager.
4. Under **Account, Access Tokens**, create a token with a short expiry.

## 2. Prepare a separate checkout

Linking writes to `supabase/.temp`, and the local stack check refuses a linked checkout. Use a worktree of its own, on the
commit under test, and a PowerShell window of your own:

```powershell
git worktree add --detach out\telemetry-dev <commit>
cd out\telemetry-dev
$s = Read-Host "Supabase access token" -AsSecureString
$env:SUPABASE_ACCESS_TOKEN = [Net.NetworkCredential]::new('', $s).Password
$ref = "<project ref>"
```

Confirm the target before anything is written. The name must be `gao-telemetry-dev` and the region `eu-central-1`:

```powershell
npx --yes supabase@3.3.0 projects list
```

## 3. Apply the migrations

```powershell
npx --yes supabase@3.3.0 link --project-ref $ref       # asks for the database password
npx --yes supabase@3.3.0 db push --dry-run             # must list exactly the three telemetry migrations
npx --yes supabase@3.3.0 db push
```

## 4. Deploy the Edge Functions

```powershell
npx --yes supabase@3.3.0 functions deploy submit-telemetry --no-verify-jwt --use-api
npx --yes supabase@3.3.0 functions deploy delete-telemetry --no-verify-jwt --use-api
```

## 5. Run the verification

```powershell
node supabase/tests/remote/verify.mjs --project-ref $ref --confirm-not-production $ref
```

Exit code 0 means every check passed, 1 that one failed, and 2 that it refused to start and wrote nothing. It sends
synthetic events only and removes them afterwards. It needs about a minute. It checks:

| Topic | How |
|---|---|
| **Database region** | `region` from the Management API must be `eu-central-1` |
| **Edge Function region** | `x-sb-edge-region` on 10 responses, sent with `x-region: eu-central-1`, must all be `eu-central-1` |
| Endpoints | Accepted, duplicate, unknown field, free text, NUL, malformed JSON, oversized body, wrong method, all without a key |
| Grants and RLS | The public key is refused by the database (`42501`) on both tables and all three functions, and the catalog audit (`supabase/tests/audit.sql`) finds no problem |
| Deletion | Removes that installation only, and an unknown ID gets the same answer |
| Retention | The job is scheduled. The purge removes a row past 12 months and keeps one just inside |

If the function region differs or the header is missing, **stop**. The design in section 6 of `docs/telemetry.md` relies on
pinning to Frankfurt and has to be reconsidered.

Not covered: the concurrency checks stay on the local stack, because they would spend the project's global limit.

## 6. Check that the scheduled job really ran

The job runs daily at 03:17 UTC. After that time, run step 5 again. `retention_job_last_run` in its evidence must show
`succeeded` with a start time after the deployment. "Has not run yet" is not a pass.

## 7. Clean up

```powershell
npx --yes supabase@3.3.0 unlink
Remove-Item Env:SUPABASE_ACCESS_TOKEN
cd ..\..
git worktree remove --force out\telemetry-dev
```

Revoke the access token in the dashboard. Keep the project for further tests, or delete it.

## Evidence

Fill this in from the tool's output. Use the first four characters of the ref at most. A passing row needs a date.

| Check | Result | Date, commit |
|---|---|---|
| Project is a non-production project on the Free plan (owner) | not run | |
| Database region from the Management API | not run | |
| Edge Function regions seen in `x-sb-edge-region` | not run | |
| `verify.mjs`: passed, failed | not run | |
| Scheduled retention job: last status and time | not run | |

## What this does not prove

- That **all** processing stays in the EU. Supabase's DPA allows processing wherever it or its subprocessors have
  facilities, and the two regions above cover only the database and the function run.
- What the provider's logs and backups hold. See section 12 of `docs/telemetry.md`.
- Anything about production, or about the Windows client.
- The hosted parts of `verify.mjs` (the Management API calls and the region checks) have not run yet. Its other checks run
  on the local stack in every `supabase/tests/stack/run.mjs`.
