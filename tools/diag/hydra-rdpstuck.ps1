#Requires -RunAsAdministrator
<#
.SYNOPSIS
    hydra-rdpstuck.ps1 v2 - seat B stuck at "Configuring remote session": capture the evidence,
    then optionally try to unstick it without a reboot.

.DESCRIPTION
    Default run is read-only. Snapshots sessions, per-session processes (zombie-session check),
    Remote Display Adapter (RdpIdd) devices and the WUDFHost carrying RdpIdd.dll, WerFault
    targets, Hydra processes, seat B .rdp redirection lines, Hydra config drift, and the last
    -Minutes of LSM / RCM / RdpCoreTS / UMDF / crash events into <OutRoot>\rdpstuck-<stamp>\,
    then prints a verdict. v2 adds the client-side RDP log (why mstsc says it disconnected) and
    who owns every port 3389 connection. summary.txt is the file to hand back.

    -Unstick captures first, then climbs a ladder of runtime-only actions, asking after each
    rung whether seat B now connects. Writes no registry, policy or service configuration.
        1  kill every mstsc.exe and WerFault.exe
        2  kill leftovers in zombie sessions (never csrss/winlogon/smss/wininit - critical), then
           try logoff on any session stuck in ConnQ/Down/Init/Reset (15 s timeout each)
        3  restart SessionEnv (Remote Desktop Configuration), which sets up each new session
        4  bounce TermService (+ UmRdpService) and Spooler, 30 s stop timeout each; a service
           that will not stop has its process killed only if that process hosts nothing else.
           Services that depended on a restarted one are started again afterwards
        5  pnputil /restart-device each present Remote Display Adapter (30 s timeout); if one
           hangs, kill the WUDFHost carrying RdpIdd.dll (drivers pooled in it restart too)

    Run it WHILE STUCK, before rebooting. The capture is the point.

.EXAMPLE
    & C:\Scripts\windows\hydra-rdpstuck.ps1

.EXAMPLE
    & C:\Scripts\windows\hydra-rdpstuck.ps1 -Unstick

.NOTES
    v1  2026-10-14  initial
    v2  2026-10-15  client-side RDP log + disconnect reasons, port 3389 owners, logoff of stuck
                    sessions (rung 2), SessionEnv restart (rung 3), dependents of a restarted
                    service are started again
#>
[CmdletBinding()]
param(
    [switch]$Unstick,
    [string]$HydraRoot  = 'C:\Programs\hydra',
    [string]$OutRoot    = 'C:\Programs\hydra-diag',
    [int]$Minutes       = 180,
    [int]$SettleSeconds = 8
)

$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false

$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$out     = Join-Path $OutRoot "rdpstuck-$stamp"
$summary = Join-Path $out 'summary.txt'
New-Item -ItemType Directory -Path $out -Force | Out-Null

$mySession    = (Get-Process -Id $PID).SessionId
$critical     = @('csrss.exe', 'winlogon.exe', 'smss.exe', 'wininit.exe', 'services.exe',
                  'lsass.exe', 'lsaiso.exe', 'System', 'System Idle Process', 'Registry',
                  'Memory Compression')
$oddStates    = @('ConnQ', 'Down', 'Init', 'Reset')
$polKey       = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services'
$tsKey        = 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server'
$script:flags = New-Object System.Collections.Generic.List[string]

# ------------------------------------------------------------------ helpers

function Say {
    param([string]$Text = '', [ConsoleColor]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
    Add-Content -LiteralPath $summary -Value $Text -Encoding utf8
}

function Save-Out {
    param([string]$Name, $Data)
    $Data | Out-File -LiteralPath (Join-Path $out $Name) -Width 4096 -Encoding utf8
}

function Get-FirstLine {
    param($Text)
    ("$Text" -split '\r?\n')[0]
}

function Get-RegValue {
    param([string]$Path, [string]$Name)
    try { (Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction Stop).$Name } catch { $null }
}

function Get-RdpIddHosts {
    $lines = @(& tasklist /m RdpIdd.dll /fo csv 2>$null)
    if ($lines.Count -and $lines[0] -like '"*') { $lines | ConvertFrom-Csv }
}

function Get-Snapshot {
    $raw  = & qwinsta 2>&1 | Out-String
    $sess = @(foreach ($line in ($raw -split '\r?\n')) {
        if ($line -match '\s(\d+)\s+(Active|ConnQ|Conn|Shadow|Disc|Idle|Listen|Reset|Down|Init)\b') {
            $id = [int]$Matches[1]
            $st = $Matches[2]
            [pscustomobject]@{
                Id      = $id
                State   = $st
                Console = ($line -match '^\s*>?\s*console\s')
                Line    = $line.TrimEnd()
            }
        }
    })
    $live  = @($sess | Where-Object State -ne 'Listen' | ForEach-Object Id)
    $procs = @(Get-CimInstance Win32_Process |
        Select-Object ProcessId, ParentProcessId, SessionId, Name, CreationDate, ExecutablePath, CommandLine)

    # Processes in a session qwinsta no longer lists = zombie session. Session 0 and our own
    # session are never candidates, and if qwinsta parsing failed nothing is classified.
    $zomb = @()
    if ($live -contains $mySession) {
        $zomb = @($procs |
            Where-Object { $_.SessionId -notin $live -and $_.SessionId -notin @(0, $mySession) } |
            ForEach-Object SessionId | Sort-Object -Unique)
    }

    $rda = @(Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like 'SWD\REMOTEDISPLAYENUM\*' } |
        ForEach-Object {
            $orphan = $false
            if ($_.InstanceId -match 'SESSIONID_([0-9A-Fa-f]+)') {
                # Suffix may be decimal or hex: orphaned only if neither reading is a live session.
                $tag    = $Matches[1]
                $dec    = 0
                $isDec  = [int]::TryParse($tag, [ref]$dec)
                $hex    = [Convert]::ToInt32($tag, 16)
                $orphan = ((-not $isDec) -or ($dec -notin $live)) -and ($hex -notin $live)
            }
            [pscustomobject]@{
                InstanceId = $_.InstanceId
                Status     = $_.Status
                Present    = $_.Present
                Problem    = $_.Problem
                Orphan     = $orphan
            }
        })

    [pscustomobject]@{
        Raw      = $raw
        Sessions = $sess
        Live     = $live
        Procs    = $procs
        Zombies  = $zomb
        Hosts    = @(Get-RdpIddHosts)
        Rda      = $rda
    }
}

function Save-Snapshot {
    param($S, [string]$Prefix = '')
    Save-Out "${Prefix}sessions.txt" $S.Raw
    $S.Procs | Sort-Object SessionId, Name |
        Export-Csv -LiteralPath (Join-Path $out "${Prefix}processes.csv") -NoTypeInformation -Encoding utf8
    Save-Out "${Prefix}rda-devices.txt" ($S.Rda | Format-Table -AutoSize | Out-String -Width 4096)
    Save-Out "${Prefix}rdpidd-hosts.txt" ($S.Hosts | Format-Table -AutoSize | Out-String -Width 4096)
}

function Save-Events {
    $since = (Get-Date).AddMinutes(-$Minutes)
    $specs = [ordered]@{
        'evt-lsm.txt'     = @{ LogName = 'Microsoft-Windows-TerminalServices-LocalSessionManager/Operational'; StartTime = $since }
        'evt-rcm.txt'     = @{ LogName = 'Microsoft-Windows-TerminalServices-RemoteConnectionManager/Operational'; StartTime = $since }
        'evt-rdpcore.txt' = @{ LogName = 'Microsoft-Windows-RemoteDesktopServices-RdpCoreTS/Operational'; StartTime = $since }
        'evt-rdpclient.txt' = @{ LogName = 'Microsoft-Windows-TerminalServices-RDPClient/Operational'; StartTime = $since }
        'evt-system.txt'  = @{ LogName = 'System'; StartTime = $since; Level = 1, 2, 3 }
        'evt-umdf.txt'    = @{ LogName = 'System'; StartTime = $since; ProviderName = 'Microsoft-Windows-DriverFrameworks-UserMode' }
        'evt-app.txt'     = @{ LogName = 'Application'; StartTime = $since; Level = 1, 2, 3 }
        'evt-wer.txt'     = @{ LogName = 'Application'; StartTime = $since; ProviderName = 'Windows Error Reporting' }
    }
    $res = @{}
    foreach ($k in $specs.Keys) {
        $ev = @(Get-WinEvent -FilterHashtable $specs[$k] -ErrorAction SilentlyContinue | Sort-Object TimeCreated)
        Save-Out $k ($ev | Format-List TimeCreated, Id, LevelDisplayName, ProviderName, Message | Out-String -Width 4096)
        $res[$k] = $ev
    }
    $res
}

function Show-Verdict {
    param($S, $Ev)

    Say ''
    Say '== sessions (qwinsta) =='
    $S.Raw -split '\r?\n' | Where-Object { $_.Trim() } | ForEach-Object { Say "  $_" }
    foreach ($o in @($S.Sessions | Where-Object { $_.State -in $oddStates })) {
        $names = @($S.Procs | Where-Object SessionId -eq $o.Id | ForEach-Object { "$($_.Name)[$($_.ProcessId)]" }) -join ' '
        if (-not $names) { $names = 'none' }
        Say "  session $($o.Id) is $($o.State) - never finished connecting; its processes: $names" 'Yellow'
        $script:flags.Add("session $($o.Id) stuck in $($o.State) - -Unstick rung 2 (logoff)")
    }

    Say ''
    Say '== zombie sessions (processes in sessions qwinsta no longer lists) =='
    if (-not $S.Zombies.Count) { Say '  none' 'Green' }
    foreach ($z in $S.Zombies) {
        $names = @($S.Procs | Where-Object SessionId -eq $z | Sort-Object Name |
            ForEach-Object { "$($_.Name)[$($_.ProcessId)]" }) -join ' '
        Say "  session ${z}: $names" 'Red'
        $script:flags.Add("zombie session $z - -Unstick rung 2")
    }

    Say ''
    Say '== WerFault (a crash reporter holds the crashed process open) =='
    $wer = @($S.Procs | Where-Object { $_.Name -in 'WerFault.exe', 'WerFaultSecure.exe' })
    if (-not $wer.Count) { Say '  none' 'Green' }
    foreach ($w in $wer) {
        $target = ''
        if ("$($w.CommandLine)" -match '-p\s+(\d+)') {
            $tp = [int]$Matches[1]
            $t  = $S.Procs | Where-Object ProcessId -eq $tp | Select-Object -First 1
            $target = if ($t) { "-> $($t.Name)[$tp] session $($t.SessionId)" } else { "-> pid $tp (gone)" }
        }
        Say "  $($w.Name)[$($w.ProcessId)] session $($w.SessionId) $target" 'Yellow'
        $script:flags.Add('WerFault holding a crashed process - -Unstick rung 1')
    }

    Say ''
    Say '== mstsc and Hydra processes =='
    foreach ($m in @($S.Procs | Where-Object Name -eq 'mstsc.exe')) {
        Say "  mstsc[$($m.ProcessId)] session $($m.SessionId) since $($m.CreationDate): $($m.CommandLine)"
    }
    foreach ($h in @($S.Procs | Where-Object { "$($_.ExecutablePath)" -like "$HydraRoot\*" })) {
        Say "  $($h.Name)[$($h.ProcessId)] session $($h.SessionId)"
    }

    Say ''
    Say '== Remote Display Adapter (RdpIdd) =='
    $present = @($S.Rda | Where-Object Present)
    Say "  devices: $($S.Rda.Count) known, $($present.Count) present"
    foreach ($d in $present) {
        $bad  = ($d.Status -ne 'OK') -or $d.Orphan
        $note = if ($d.Orphan) { '  [no live session]' } else { '' }
        Say ('  {0}  status={1} problem={2}{3}' -f $d.InstanceId, $d.Status, $d.Problem, $note) $(if ($bad) { 'Red' } else { 'Gray' })
        if ($bad) { $script:flags.Add('Remote Display Adapter unhealthy or orphaned - -Unstick rung 5') }
    }
    $hostTxt = 'nothing'
    if ($S.Hosts.Count) { $hostTxt = @($S.Hosts | ForEach-Object { "$($_.'Image Name')[$($_.PID)]" }) -join ' ' }
    Say "  RdpIdd.dll loaded in: $hostTxt"

    $umdf   = @($Ev['evt-umdf.txt'])
    $rdUmdf = @($umdf | Where-Object { $_.ToXml() -match 'RDPIDD|Remote Display' })
    $crash  = @($Ev['evt-app.txt'] | Where-Object {
        $_.Id -in 1000, 1002 -and "$($_.Message)" -match 'WUDFHost|dwm\.exe|mstsc\.exe|LogonUI|rdpidd|rdpclip' })
    Say ''
    Say "== events, last $Minutes min (full text in evt-*.txt) =="
    Say "  UMDF: $($umdf.Count), naming the Remote Display Adapter: $($rdUmdf.Count)" $(if ($rdUmdf.Count) { 'Red' } else { 'Gray' })
    foreach ($e in @($rdUmdf | Select-Object -Last 3)) { Say "    $($e.TimeCreated)  id $($e.Id)  $(Get-FirstLine $e.Message)" 'Red' }
    if ($rdUmdf.Count) { $script:flags.Add('UMDF crash/offline events for RdpIdd - -Unstick rung 5') }
    Say "  crashes/hangs on the RDP path (Application 1000/1002): $($crash.Count)" $(if ($crash.Count) { 'Red' } else { 'Gray' })
    foreach ($e in @($crash | Select-Object -Last 5)) { Say "    $($e.TimeCreated)  id $($e.Id)  $(Get-FirstLine $e.Message)" 'Red' }
    if ($crash.Count) { $script:flags.Add('crash on the RDP path - see evt-app.txt and evt-wer.txt') }

    $cl = @($Ev['evt-rdpclient.txt'] | Where-Object Id -eq 1026)
    Say ''
    Say "== client-side disconnects (RDPClient 1026, last $Minutes min): $($cl.Count) =="
    if (-not $cl.Count) { Say '  none' 'Green' }
    foreach ($e in @($cl | Select-Object -Last 6)) { Say "  $($e.TimeCreated)  $(Get-FirstLine $e.Message)" 'Yellow' }
    if (@($cl | Where-Object { $_.Message -match 'Reason= 2308' }).Count) {
        $script:flags.Add('mstsc reports 2308 (socket closed): the loopback connection was reset - run hydra-dropcheck')
    }

    Say ''
    Say '== port 3389 connections (who owns each end) =='
    $tcp = @(Get-NetTCPConnection -ErrorAction SilentlyContinue |
        Where-Object { (3389 -in $_.LocalPort, $_.RemotePort) -and $_.State -ne 'Listen' })
    if (-not $tcp.Count) { Say '  none besides the listener - nothing is connected' }
    foreach ($c in $tcp) {
        $pn  = (Get-Process -Id $c.OwningProcess -ErrorAction SilentlyContinue).ProcessName
        $odd = ($c.OwningProcess -ne 0) -and ($pn -notin 'svchost', 'mstsc')
        Say ('  {0}:{1} -> {2}:{3}  {4}  {5}[{6}]' -f $c.LocalAddress, $c.LocalPort, $c.RemoteAddress, $c.RemotePort, $c.State, $pn, $c.OwningProcess) $(if ($odd) { 'Red' } else { 'Gray' })
        if ($odd) { $script:flags.Add("port 3389 connection owned by $pn - something is proxying RDP") }
    }

    Say ''
    Say '== services =='
    foreach ($n in 'TermService', 'UmRdpService', 'SessionEnv', 'LSM', 'Spooler', 'WerSvc') {
        $sv = Get-Service -Name $n -ErrorAction SilentlyContinue
        if ($sv) { Say ('  {0,-13} {1}' -f $n, $sv.Status) }
    }
    $tsSvc  = Get-CimInstance Win32_Service -Filter "Name='TermService'"
    $tsProc = $S.Procs | Where-Object ProcessId -eq $tsSvc.ProcessId | Select-Object -First 1
    Say "  TermService process [$($tsSvc.ProcessId)] started $($tsProc.CreationDate)"
}

function Show-RdpFiles {
    param($S)
    $paths = @()
    foreach ($m in @($S.Procs | Where-Object Name -eq 'mstsc.exe')) {
        if ("$($m.CommandLine)" -match '([A-Za-z]:\\[^"]*?\.rdp)') { $paths += $Matches[1] }
    }
    $paths += @(Get-ChildItem -LiteralPath $HydraRoot -Filter *.rdp -Recurse -Depth 3 -File -ErrorAction SilentlyContinue |
        ForEach-Object FullName)
    $paths = @($paths | Where-Object { $_ } | Sort-Object -Unique)
    Say ''
    Say '== seat B .rdp redirection (per-connection settings, not system config) =='
    if (-not $paths.Count) { Say '  no .rdp found (mstsc not running, none under HydraRoot)'; return }
    foreach ($p in $paths) {
        Say "  $p"
        if (-not (Test-Path -LiteralPath $p)) { Say '    (file gone)'; continue }
        Get-Content -LiteralPath $p -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^(redirect|drivestoredirect|devicestoredirect|camerastoredirect|usbdevicestoredirect|audiomode|audiocapturemode|use multitransport)' } |
            ForEach-Object { Say "    $_" }
    }
}

function Show-Drift {
    $pdeny = Get-RegValue $polKey 'fDenyTSConnections'
    $deny  = if ($null -ne $pdeny) { $pdeny } else { Get-RegValue $tsKey 'fDenyTSConnections' }
    $psspu = Get-RegValue $polKey 'fSingleSessionPerUser'
    $sspu  = if ($null -ne $psspu) { $psspu } else { Get-RegValue $tsKey 'fSingleSessionPerUser' }
    $checks = @(
        @{ N = 'bEnumerateHWBeforeSW (policy)'; V = (Get-RegValue $polKey 'bEnumerateHWBeforeSW'); Want = 'absent - present puts seat B on the 1660 Ti, whose encoder crashed RdpIdd'; Ok = { param($v) $null -eq $v } }
        @{ N = 'fClientDisableUDP (client policy)'; V = (Get-RegValue "$polKey\Client" 'fClientDisableUDP'); Want = '1 (TCP-only)'; Ok = { param($v) $v -eq 1 } }
        @{ N = 'fDenyTSConnections (effective)'; V = $deny; Want = '0'; Ok = { param($v) $v -eq 0 } }
        @{ N = 'fSingleSessionPerUser (effective)'; V = $sspu; Want = '0 (last confirmed value)'; Ok = { param($v) $v -eq 0 } }
        @{ N = 'TermService ServiceDll'; V = (Get-RegValue 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' 'ServiceDll'); Want = '...\termsrv.dll (RDP Wrapper retired)'; Ok = { param($v) "$v" -like '*\termsrv.dll' } }
        @{ N = 'state\termsrv-UNPATCHED marker'; V = (Test-Path -LiteralPath (Join-Path $HydraRoot 'state\termsrv-UNPATCHED')); Want = 'absent'; Ok = { param($v) -not $v } }
    )
    Say ''
    Say '== Hydra config drift =='
    foreach ($c in $checks) {
        $shown = if ($null -eq $c.V) { '<absent>' } else { "$($c.V)" }
        if (& $c.Ok $c.V) {
            Say ('  OK     {0} = {1}' -f $c.N, $shown) 'Green'
        } else {
            Say ('  DRIFT  {0} = {1}   want {2}' -f $c.N, $shown, $c.Want) 'Red'
            $script:flags.Add("config drift: $($c.N)")
        }
    }
    foreach ($n in 'AVC444ModePreferred', 'AVCHardwareEncodePreferred', 'SelectTransport', 'SelectNetworkDetect', 'fEnableWddmDriver') {
        $v = Get-RegValue $polKey $n
        if ($null -ne $v) { Say "  info   $n = $v (policy)" }
    }
    $v = Get-RegValue $tsKey 'bAllowFastReconnect'
    if ($null -ne $v) { Say "  info   bAllowFastReconnect = $v" }
}

# ------------------------------------------------------------------ capture

$os       = Get-CimInstance Win32_OperatingSystem
$tdll     = Get-Item -LiteralPath "$env:SystemRoot\System32\termsrv.dll" -ErrorAction SilentlyContinue
$vi       = $tdll.VersionInfo
$pending  = ((Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') -or
             (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'))

Say "hydra-rdpstuck v2  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  ->  $out" 'Cyan'
Say "  build $($os.Version)  boot $($os.LastBootUpTime)  reboot pending: $pending"
Say ('  termsrv.dll {0}.{1}.{2}.{3}  written {4}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart, $tdll.LastWriteTime)

$snap   = Get-Snapshot
Save-Snapshot $snap
$events = Save-Events
Show-Verdict $snap $events
Show-RdpFiles $snap
Show-Drift

Say ''
Say '== verdict ==' 'Cyan'
if ($script:flags.Count) {
    $script:flags | Sort-Object -Unique | ForEach-Object { Say "  * $_" 'Yellow' }
} else {
    Say '  nothing obvious - hand back summary.txt plus evt-lsm.txt and evt-rdpcore.txt' 'Yellow'
}

if (-not $Unstick) {
    Say ''
    Say "Read-only run. If seat B is stuck right now, re-run with -Unstick. Hand back: $summary" 'Cyan'
    return
}

# ------------------------------------------------------------------ unstick

function Confirm-SeatB {
    param([int]$Rung)
    $a = Read-Host "Rung $Rung done. Relaunch seat B now - did it connect? [y/N]"
    if ($a -match '^(y|yes)$') { Say "RESULT: seat B connected after rung $Rung" 'Green'; return $true }
    Say "  rung $Rung did not fix it" 'Yellow'
    return $false
}

function Stop-Targets {
    param([object[]]$Targets)
    foreach ($t in $Targets) {
        Stop-Process -Id $t.ProcessId -Force -ErrorAction SilentlyContinue
        Say "  killed $($t.Name)[$($t.ProcessId)] session $($t.SessionId)"
    }
    Start-Sleep -Seconds $SettleSeconds
    foreach ($t in $Targets) {
        if (Get-Process -Id $t.ProcessId -ErrorAction SilentlyContinue) {
            Say "  SURVIVED $($t.Name)[$($t.ProcessId)] - stuck in a kernel wait; user mode cannot free it" 'Red'
        }
    }
}

function Get-RunningDependents {
    param([string]$Name)
    $seen  = New-Object System.Collections.Generic.List[string]
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($Name)
    while ($queue.Count) {
        $n = $queue.Dequeue()
        foreach ($d in @(Get-Service -Name $n -DependentServices -ErrorAction SilentlyContinue)) {
            if ($d.Status -eq 'Running' -and -not $seen.Contains($d.Name)) {
                $seen.Add($d.Name)
                $queue.Enqueue($d.Name)
            }
        }
    }
    return @($seen)
}

function Restart-ServiceBounded {
    param([string]$Name, [int]$TimeoutSec = 30)
    if (-not (Get-Service -Name $Name -ErrorAction SilentlyContinue)) { return }
    $deps = @(Get-RunningDependents $Name)
    Stop-Service -Name $Name -Force -NoWait -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Service -Name $Name).Status -ne 'Stopped' -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if ((Get-Service -Name $Name).Status -ne 'Stopped') {
        $svcPid  = (Get-CimInstance Win32_Service -Filter "Name='$Name'").ProcessId
        $sharing = @(if ($svcPid) { Get-CimInstance Win32_Service -Filter "ProcessId=$svcPid" | ForEach-Object Name })
        if ($svcPid -and $sharing.Count -eq 1) {
            Say "  $Name would not stop in $TimeoutSec s - killing its process [$svcPid]" 'Yellow'
            Stop-Process -Id $svcPid -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 3
        } else {
            Say "  $Name would not stop; process [$svcPid] also hosts $($sharing -join ', ') - left alone" 'Red'
        }
    }
    Start-Service -Name $Name -ErrorAction SilentlyContinue
    foreach ($d in $deps) {
        Start-Service -Name $d -ErrorAction SilentlyContinue
        Say "  $Name restart: dependent $d $((Get-Service -Name $d -ErrorAction SilentlyContinue).Status)"
    }
}

function Complete-Run {
    Save-Snapshot (Get-Snapshot) 'after-'
    Say ''
    Say "Saved: $out  (summary.txt is the one to hand back)" 'Cyan'
}

Say ''
Say '== -Unstick ==' 'Cyan'
$active = @($snap.Sessions | Where-Object { $_.State -eq 'Active' -and -not $_.Console -and $_.Id -ne $mySession })
if ($active.Count) {
    $a = Read-Host "Session(s) $($active.Id -join ', ') are Active - seat B may be fine. Continue anyway? [y/N]"
    if ($a -notmatch '^(y|yes)$') { Say 'Stopped: seat B looks connected.' 'Yellow'; return }
}

Say ''
Say '-- rung 1: mstsc + WerFault' 'Cyan'
$t1 = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -in 'mstsc.exe', 'WerFault.exe' } |
    Select-Object ProcessId, Name, SessionId)
if ($t1.Count) {
    Stop-Targets $t1
    if (Confirm-SeatB 1) { Complete-Run; return }
} else {
    Say '  nothing to kill'
}

Say ''
Say '-- rung 2: zombie leftovers, then logoff of stuck sessions' 'Cyan'
$s2 = Get-Snapshot
$t2 = @($s2.Procs | Where-Object { $_.SessionId -in $s2.Zombies -and $_.Name -notin $critical } |
    Select-Object ProcessId, Name, SessionId)
$stuck2 = @($s2.Sessions | Where-Object { $_.State -in $oddStates -and $_.Id -ne $mySession })
$acted  = $false
if ($t2.Count) {
    Stop-Targets $t2
    $acted = $true
} elseif ($s2.Zombies.Count) {
    Say "  zombie session(s) $($s2.Zombies -join ', ') hold only critical processes - a driver is pinning them" 'Red'
}
foreach ($st in $stuck2) {
    $lo = Start-Process -FilePath "$env:SystemRoot\System32\logoff.exe" -ArgumentList "$($st.Id)" -NoNewWindow -PassThru
    $null = $lo.Handle
    if ($lo.WaitForExit(15000)) {
        Say "  logoff $($st.Id) ($($st.State)): exit $($lo.ExitCode)"
    } else {
        Say "  logoff $($st.Id) hung >15 s - the session is wedged in a kernel wait" 'Red'
        Stop-Process -Id $lo.Id -Force -ErrorAction SilentlyContinue
    }
    $acted = $true
}
if ($acted) {
    Start-Sleep -Seconds $SettleSeconds
    Say (& qwinsta 2>&1 | Out-String).TrimEnd()
    if (Confirm-SeatB 2) { Complete-Run; return }
} elseif (-not $s2.Zombies.Count) {
    Say '  no zombie or stuck sessions'
}

Say ''
Say '-- rung 3: SessionEnv (Remote Desktop Configuration)' 'Cyan'
Restart-ServiceBounded 'SessionEnv'
Say "  SessionEnv $((Get-Service SessionEnv).Status)  TermService $((Get-Service TermService).Status)  UmRdpService $((Get-Service UmRdpService).Status)"
if (Confirm-SeatB 3) { Complete-Run; return }

Say ''
Say '-- rung 4: TermService + UmRdpService + Spooler' 'Cyan'
Restart-ServiceBounded 'TermService'
Start-Service -Name UmRdpService -ErrorAction SilentlyContinue
Restart-ServiceBounded 'Spooler'
Say "  TermService $((Get-Service TermService).Status)  UmRdpService $((Get-Service UmRdpService).Status)  Spooler $((Get-Service Spooler).Status)"
if (Test-Path -LiteralPath (Join-Path $HydraRoot 'state\termsrv-UNPATCHED')) {
    Say '  termsrv-guard marker present - concurrency patch is NOT applied' 'Red'
}
if (Confirm-SeatB 4) { Complete-Run; return }

Say ''
Say '-- rung 5: Remote Display Adapter' 'Cyan'
$devs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'SWD\REMOTEDISPLAYENUM\*' })
$hung = $false
foreach ($d in $devs) {
    $log  = Join-Path $out ('pnputil-' + ($d.InstanceId -replace '[^A-Za-z0-9]+', '_') + '.txt')
    $proc = Start-Process -FilePath pnputil.exe -ArgumentList '/restart-device', $d.InstanceId -NoNewWindow -PassThru -RedirectStandardOutput $log
    $null = $proc.Handle
    if ($proc.WaitForExit(30000)) {
        Say "  restart-device $($d.InstanceId): exit $($proc.ExitCode)"
    } else {
        Say "  pnputil hung >30 s on $($d.InstanceId) - that device is wedged" 'Red'
        $hung = $true
    }
}
if ($hung) {
    foreach ($h in @(Get-RdpIddHosts)) {
        Say "  killing WUDFHost[$($h.PID)] carrying RdpIdd.dll" 'Yellow'
        Stop-Process -Id ([int]$h.PID) -Force -ErrorAction SilentlyContinue
    }
}
if ($devs.Count) {
    if (Confirm-SeatB 5) { Complete-Run; return }
} else {
    Say '  no present Remote Display Adapter to restart'
}

Say ''
Say 'Out of runtime moves - reboot. The capture says what to go after next.' 'Red'
Complete-Run
