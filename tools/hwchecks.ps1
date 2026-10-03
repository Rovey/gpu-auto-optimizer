# Drives the hardware checks of docs/hardware-checks.md that run gao.exe:
# 45 (the --probe part), 46, 48, 49/52 (full runs), 50, 51 and 53.
# Checks 54-56 need the window and are done by hand; -Checks trayinfo only
# records which tray app is running.
#
# Usage, from an elevated PowerShell in the repository, after a Release build:
#   Set-ExecutionPolicy -Scope Process Bypass
#   .\tools\hwchecks.ps1 -Checks 45probe -Out .\out\hw\45
#   .\tools\hwchecks.ps1 -Checks 51 -Out .\out\hw\51
# -Checks takes a comma-separated list of: trayinfo, 45probe, 53, 48, 51,
# full-best, full-max, 46, 50. Each run writes summary.txt, the gao log, the
# new journal lines and a DONE marker into -Out. The forced driver reset needs
# dxcap.exe (Windows optional feature "Graphics Tools").
#
# Must run elevated for real checks. Starts gao.exe in this console, watches
# the crash journal for trigger points, and sends Ctrl+C / forces a driver
# reset at the right moment. -Gao/-Journal/-AppDir/-TdrExe let a dry run use a
# fake gao.
#
# The search order this script expects: baseline, power, memory (journal
# entries with only "mem"), then core in steps of 15 MHz with the memory offset
# applied (entries with only "core"), then the soak (entries with both).
#
# WARNING: checks 51 and full-* cause a real driver reset: the screen goes
# black for a few seconds and programs that use the GPU can crash. Save your
# work first.
param(
    [string]$Checks = '',
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Gao = (Join-Path $PSScriptRoot '..\build\Release\gao.exe'),
    [string]$Journal = "$env:ProgramData\GpuAutoOptimizer\journal.jsonl",
    [string]$AppDir = "$env:ProgramData\GpuAutoOptimizer",
    [string]$TdrExe = 'dxcap.exe',
    [string]$TdrArgs = '-forcetdr',
    [int]$SoakHoldS = 60,
    # Failsafe for 48 and 51: the run is stopped if it applies a core candidate
    # at or above this before the check's own trigger.
    [int]$MaxCore = 240,
    # 51: the reset is forced during this core candidate (6 = core +90).
    [int]$TdrAtCoreCandidate = 6,
    # Dry runs shorten every wait by this factor.
    [double]$TimeScale = 1.0
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$summary = Join-Path $Out 'summary.txt'
$doneMarker = Join-Path $Out 'DONE'
Remove-Item $doneMarker -ErrorAction SilentlyContinue

function Secs([double]$s) { [int][Math]::Max(1, [Math]::Ceiling($s * $TimeScale)) }

# Never throws: a reader holding summary.txt open must not end the script.
function Note($m) {
    $line = '{0}  {1}' -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    for ($i = 0; $i -lt 10; $i++) {
        try { Add-Content -Path $summary -Value $line -Encoding utf8 -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 200 }
    }
}

Add-Type -Namespace Native -Name Con -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
'@

# Ctrl+C to every process on this console. This script ignores it for the
# moment it is sent; gao.exe was started before the ignore flag was set, so it
# does not inherit the flag and its own handler runs.
function Send-CtrlC {
    [Native.Con]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
    $ok = [Native.Con]::GenerateConsoleCtrlEvent(0, 0)
    Start-Sleep -Milliseconds 800
    [Native.Con]::SetConsoleCtrlHandler([IntPtr]::Zero, $false) | Out-Null
    return $ok
}

function Journal-Lines {
    if (-not (Test-Path $Journal)) { return @() }
    for ($i = 0; $i -lt 5; $i++) {
        try { return @(Get-Content $Journal -ErrorAction Stop) } catch { Start-Sleep -Milliseconds 100 }
    }
    return @()
}

# Parsed journal entries added since line index $from. Emits one object per
# entry; callers wrap the call in @().
function New-Entries($from) {
    $all = @(Journal-Lines)
    if ($all.Count -le $from) { return }
    foreach ($l in $all[$from..($all.Count - 1)]) {
        try { $l | ConvertFrom-Json } catch { }
    }
}
function Has($o, $name) { $o.PSObject.Properties.Name -contains $name }
function Begins($from) { @(New-Entries $from) | Where-Object { (Has $_ 'state') -and $_.state -eq 'begin' } }
function Core-Begins($from) { @(Begins $from) | Where-Object { (Has $_ 'core') -and -not (Has $_ 'mem') } }
function Mem-Begins($from) { @(Begins $from) | Where-Object { (Has $_ 'mem') -and -not (Has $_ 'core') } }
function Soak-Begins($from) { @(Begins $from) | Where-Object { (Has $_ 'mem') -and (Has $_ 'core') } }
# @($null).Count is 1 in PowerShell: an empty pipeline must count as zero.
function Count-Of($items) { if ($null -eq $items) { 0 } else { @($items).Count } }
function Open-Begins($from) {
    $e = @(New-Entries $from)
    $closed = @($e | Where-Object { (Has $_ 'state') -and $_.state -eq 'complete' } | ForEach-Object { $_.id })
    @($e | Where-Object { (Has $_ 'state') -and $_.state -eq 'begin' -and ($closed -notcontains $_.id) })
}
function Max-Core($from) {
    $c = @(Core-Begins $from | ForEach-Object { $_.core })
    if ($c.Count) { ($c | Measure-Object -Maximum).Maximum } else { -1 }
}

function Start-Gao($arguments, $log) {
    if ($Gao -like '*.ps1') {
        $p = Start-Process -FilePath 'powershell.exe' -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$Gao`" $arguments" `
            -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    } else {
        $p = Start-Process -FilePath $Gao -ArgumentList $arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    }
    $null = $p.Handle   # keeps ExitCode readable after exit
    return $p
}

# Runs a short gao command with a time limit; its output goes to $log.
# Returns the exit code, or $null when it did not finish.
function Run-Short($arguments, $log, [int]$timeoutS = 90) {
    try {
        $p = Start-Gao $arguments $log
        if (-not $p.WaitForExit($timeoutS * 1000)) { $p.Kill(); Note "gao $arguments did not finish within $timeoutS s; killed"; return $null }
        return $p.ExitCode
    } catch { Note "gao $arguments could not run: $($_.Exception.Message)"; return $null }
}

# Waits for $cond. $maxCore (>= 0) is a failsafe: if the run applies a core
# candidate at or above it before $cond holds, the wait ends with 'maxcore'.
function Wait-Until([scriptblock]$cond, [int]$timeoutS, $proc, $from = 0, [int]$maxCore = -1) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeoutS) {
        if ($proc.HasExited) { return 'exited' }
        if (& $cond) { return 'ok' }
        if ($maxCore -ge 0 -and (Max-Core $from) -ge $maxCore) { return 'maxcore' }
        Start-Sleep -Milliseconds 250
    }
    return 'timeout'
}

# Sends Ctrl+C and measures how long gao takes to exit. Falls back to a kill
# plus --reset if it does not exit.
function Stop-Run($proc, $label) {
    if ($proc.HasExited) { Note "${label}: process had already exited (code $($proc.ExitCode))"; return }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $sent = Send-CtrlC
    $exited = $proc.WaitForExit(90000)
    $sw.Stop()
    if ($exited) {
        Note ("${label}: Ctrl+C sent={0}; exited after {1:N1} s (includes the 0.8 s the sender waits), exit code {2}" -f $sent, $sw.Elapsed.TotalSeconds, $proc.ExitCode)
    } else {
        Note "${label}: DID NOT EXIT within 90 s of Ctrl+C -- killing and resetting"
        try { $proc.Kill() } catch { }
        $null = Run-Short '--reset' (Join-Path $Out "$label-forced-reset.log")
        $null = Run-Short '--fan auto' (Join-Path $Out "$label-forced-fan.log")
    }
}

function Wrap-Up($label, $from, $log) {
    Start-Sleep -Seconds 1
    $null = Run-Short '--status' (Join-Path $Out "$label-status.log")
    $all = @(Journal-Lines)
    if ($all.Count -gt $from) { $all[$from..($all.Count - 1)] | Out-File (Join-Path $Out "$label-journal.log") -Encoding utf8 }
    $open = @(Open-Begins $from)
    Note ("${label}: journal entries added: {0}; open (begin without complete): {1}; highest core candidate: {2}" -f ($all.Count - $from), $open.Count, (Max-Core $from))
    if (Test-Path $log) {
        foreach ($t in @(Get-Content $log -Tail 5)) { Note "${label}: log> $t" }
    }
}

# Which tray app runs, and from where. Checks 51 and 52 ask for this: an
# installed v0.2.0 with its window open can itself crash at a forced reset.
function Note-Tray($label) {
    $procs = @(Get-CimInstance Win32_Process -Filter "Name='GpuAutoOptimizer.exe'" -ErrorAction SilentlyContinue)
    if (-not $procs.Count) { Note "${label}: no tray app (GpuAutoOptimizer.exe) is running"; return }
    foreach ($t in $procs) {
        $ver = ''
        try { $ver = (Get-Item $t.ExecutablePath -ErrorAction Stop).VersionInfo.FileVersion } catch { }
        $path = $t.ExecutablePath
        if (-not $path) { $path = '(path not readable: this shell is not elevated)' }
        Note "${label}: tray app running: pid $($t.ProcessId), $path, version $ver"
    }
}

function Dump-Count { @(Get-ChildItem -Path $AppDir -Filter 'crash-*.dmp' -ErrorAction SilentlyContinue).Count }

# Driver resets Windows recorded since $since: the "display driver stopped
# responding and has recovered" event (Display 4101) and nvlddmkm's own events.
function Note-ResetEvents($label, $since) {
    $ev = @()
    try {
        $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction Stop |
            Where-Object { $_.ProviderName -eq 'nvlddmkm' -or ($_.ProviderName -eq 'Display' -and $_.Id -eq 4101) })
    } catch { }
    $recovered = @($ev | Where-Object { $_.ProviderName -eq 'Display' }).Count
    Note "${label}: Windows System log since the run started: $recovered driver reset(s) (Display 4101), $($ev.Count - $recovered) nvlddmkm event(s)"
    foreach ($e in @($ev | Sort-Object TimeCreated)) { Note ("${label}: event> {0:HH:mm:ss} {1} {2}" -f $e.TimeCreated, $e.ProviderName, $e.Id) }
}

# Reports whether the lines a recovery must log are there, in order.
function Note-RecoveryLines($label, $log) {
    $want = @('the driver was reset; reconnecting', 'resting 20 s before the card is loaded again', 'preparing the stress load',
              'checking that the card is back', 'health:')
    $lines = @()
    if (Test-Path $log) { $lines = @(Get-Content $log) }
    $at = 0; $missing = @()
    foreach ($w in $want) {
        $found = -1
        for ($i = $at; $i -lt $lines.Count; $i++) { if ($lines[$i].Contains($w)) { $found = $i; break } }
        if ($found -lt 0) { $missing += $w } else { $at = $found + 1 }
    }
    if ($missing.Count) { Note "${label}: recovery lines MISSING or out of order: $($missing -join ' | ')" }
    else { Note "${label}: recovery lines present in order (reconnect, rest, prepare, check, health)" }
    foreach ($l in @($lines | Where-Object { $_ -match 'health:|the driver reset|RESULT:|driver resets during this run|could not be rebuilt|not ready yet' })) { Note "${label}: log> $l" }
}

function Note-AfterReset($label, $since, $log, $dumpsBefore) {
    Note-ResetEvents $label $since
    Note-RecoveryLines $label $log
    $dumps = Dump-Count
    Note "${label}: crash dumps in ${AppDir}: $dumpsBefore before, $dumps after"
    Note-Tray "$label (after)"
    Note "${label}: NOW CHECK BY HAND within one minute: open windows visible and usable, a new program starts, no sign-out needed; note programs that crashed."
}

$want = @($Checks.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
Note "START checks: $($want -join ', ')  gao: $Gao  journal: $Journal"
Note-Tray 'start'

try {
    if ($want -contains 'trayinfo') { Note "trayinfo: crash dumps in ${AppDir}: $(Dump-Count)" }

    # 45, the --probe part: the driver's offset range, read-only. To be read
    # again after every driver update.
    if ($want -contains '45probe') {
        $log = Join-Path $Out '45-probe.log'
        $code = Run-Short '--probe' $log
        Note "45probe: exit code $code"
        if (Test-Path $log) { foreach ($t in @(Get-Content $log)) { Note "45probe: log> $t" } }
        $null = Run-Short '--status' (Join-Path $Out '45-status.log')
    }

    # 53: the stress load is rebuilt on a healthy driver. Loads the card for
    # two 3 s runs; changes no setting.
    if ($want -contains '53') {
        $log = Join-Path $Out '53-stress-recreate.log'
        $code = Run-Short '--stress-recreate' $log 120
        Note "53: exit code $code (0 = both runs STABLE and the rebuild succeeded)"
        if (Test-Path $log) { foreach ($t in @(Get-Content $log)) { Note "53: log> $t" } }
    }

    # 48: Ctrl+C during a clock probe. The memory search comes first, so the
    # first clock candidate is a memory one.
    if ($want -contains '48') {
        $label = '48b-clock'; $log = Join-Path $Out '48b-optimize.log'
        $from = (Journal-Lines).Count
        $p = Start-Gao '--optimize best' $log
        $r = Wait-Until { (Count-Of (Begins $from)) -ge 1 } (Secs 900) $p $from $MaxCore
        Note "48b: waited for the first clock candidate: $r"
        Start-Sleep -Milliseconds 600
        Stop-Run $p $label
        Wrap-Up $label $from $log
    }

    # 51: a forced driver reset during the core climb, at a low and harmless
    # candidate; the run is then left to end by itself. After its one reset the
    # search explores no further, so no core value above the lost one may
    # appear. Failsafe: the run is stopped if it reaches -MaxCore before the
    # reset was sent.
    if ($want -contains '51') {
        $label = '51'; $log = Join-Path $Out '51-optimize.log'
        $from = (Journal-Lines).Count
        $since = Get-Date; $dumps = Dump-Count
        $p = Start-Gao '--optimize best' $log
        $r = Wait-Until { (Count-Of (Core-Begins $from)) -ge $TdrAtCoreCandidate } (Secs 2400) $p $from $MaxCore
        Note "51: waited for core candidate number ${TdrAtCoreCandidate}: $r"
        if ($r -eq 'ok') {
            Start-Sleep -Milliseconds 700
            $lostAt = Max-Core $from
            $tdr = Start-Process -FilePath $TdrExe -ArgumentList $TdrArgs -NoNewWindow -PassThru -Wait
            Note "51: forced driver reset sent (exit code $($tdr.ExitCode)) while core +$lostAt was running"
            # The lost candidate itself is the highest allowed from here on.
            $r2 = Wait-Until { $false } (Secs 2400) $p $from ($lostAt + 1)
            Note "51: waited for the run to end by itself: $r2"
            if ($r2 -eq 'maxcore') { Note "51: FAIL -- a core value above +$lostAt was applied after the reset" }
            Note "51: highest core candidate of the run: +$(Max-Core $from) (the reset was at +$lostAt)"
        }
        if ($p.HasExited) { Note "51: gao exited on its own, code $($p.ExitCode)" } else { Stop-Run $p $label }
        Wrap-Up $label $from $log
        Note-AfterReset $label $since $log $dumps
    }

    # 52 / 49: a full run to the end; the climb crosses the real edge.
    foreach ($preset in @('best', 'max')) {
        if ($want -contains "full-$preset") {
            $label = "full-$preset"; $log = Join-Path $Out "full-$preset-optimize.log"
            $from = (Journal-Lines).Count
            $since = Get-Date; $dumps = Dump-Count
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $p = Start-Gao "--optimize $preset" $log
            $finished = $p.WaitForExit((Secs 5400) * 1000)
            $sw.Stop()
            if (-not $finished) { Note "${label}: still running after 90 min -- aborting it"; Stop-Run $p $label }
            Note ("${label}: finished={0}, exit code {1}, total {2:N0} s ({3:N1} min)" -f $finished, $p.ExitCode, $sw.Elapsed.TotalSeconds, $sw.Elapsed.TotalMinutes)
            Wrap-Up $label $from $log
            Note-AfterReset $label $since $log $dumps
        }
    }

    # 46: Ctrl+C one minute into the soak.
    if ($want -contains '46') {
        $label = '46-soak'; $log = Join-Path $Out '46-optimize.log'
        $from = (Journal-Lines).Count
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Gao '--optimize best' $log
        $r = Wait-Until { (Count-Of (Soak-Begins $from)) -ge 1 } (Secs 5400) $p $from
        Note ("46: waited for the soak to start: {0} after {1:N0} s" -f $r, $sw.Elapsed.TotalSeconds)
        if ($r -eq 'ok') { Start-Sleep -Seconds (Secs $SoakHoldS) }
        Stop-Run $p $label
        Wrap-Up $label $from $log
    }

    # 50: a hand-made ceiling at +150 must keep every core candidate below it.
    # The core climb comes after the memory search and ascends by 15 from
    # stock, so its last allowed candidate is +135: wait for it, give the run
    # time to try the next value if it wrongly would, then stop it.
    if ($want -contains '50') {
        $label = '50-ceiling'; $log = Join-Path $Out '50-optimize.log'
        $injected = '{"core":150,"id":9999,"state":"begin"}'
        Add-Content -Path $Journal -Value $injected -Encoding ascii
        try {
            $from = (Journal-Lines).Count
            $p = Start-Gao '--optimize best' $log
            $r = Wait-Until { (Max-Core $from) -ge 135 } (Secs 2400) $p $from 150
            Note "50: waited for the core climb to reach +135: $r"
            if ($r -eq 'ok') {
                $r2 = Wait-Until { $false } (Secs 40) $p $from 150
                Note "50: the 40 s after +135: $r2 (timeout = no value at or above the ceiling was applied)"
            }
            Stop-Run $p $label
            $coreTried = @(Core-Begins $from | ForEach-Object { $_.core })
            Note "50: core candidates tried: $($coreTried -join ', '); highest: $(Max-Core $from) (ceiling 150)"
            if ((Max-Core $from) -ge 150) { Note '50: FAIL -- a core value at or above the ceiling was applied' }
            Wrap-Up $label $from $log
        } finally {
            $all = @(Journal-Lines)
            $kept = @($all | Where-Object { $_ -ne $injected })
            Set-Content -Path $Journal -Value $kept -Encoding ascii
            Note "50: injected ceiling line removed; journal $($all.Count) -> $($kept.Count) lines"
        }
    }
}
catch {
    Note "SCRIPT ERROR: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
}
finally {
    Note 'END'
    Set-Content -Path $doneMarker -Value (Get-Date -Format o)
}
