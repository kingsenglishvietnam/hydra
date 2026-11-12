#requires -Version 5.1
# mode4-guard-helpers.ps1 -- functions for MODE4-RETEST-GUARD.md.
#
# Dot-source it from an ELEVATED Hydra Shell, in C:\Programs\hydra:
#
#     . .\mode4-guard-helpers.ps1
#
# Nothing runs on load. It defines:
#
#     Restart-HydraTs       stop Hydra + UmRdpService + TermService, start TermService
#     Get-ProtoSession      the hydraproto#N session, if one exists
#     Set-ProtoCreds        write Username / Domain / Password to the listener key
#     Get-HydraIddPackage   every iddseat / iddsample package in the driver store
#     Start-ProtoRun        trigger one provider connection and record what happens
#     Stop-ProtoRun         trigger file gone, provider session logged off
#
# Start-ProtoRun writes everything it prints to
#     C:\Programs\hydra\logs\mode4-guard-<label>-<stamp>.txt
# (logs\ is gitignored).

$ErrorActionPreference = 'Continue'
# PS 7.4: native stderr must not become a terminating error (see hydra7-avc.ps1).
$PSNativeCommandUseErrorActionPreference = $false

$HydraRoot   = 'C:\Programs\hydra'
$ProtoKey    = 'HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto'
$TriggerFile = 'C:\TestProtocol\createconnection.txt'
$ProviderLog = 'C:\ProgramData\Hydra\provider.log'

function Restart-HydraTs {
    # Restart-Service TermService -Force trips over UmRdpService (RDS-PROVIDER.md),
    # and TermService will not stop while the Hydra service is running.
    Stop-Service Hydra -Force -ErrorAction SilentlyContinue
    Stop-Service UmRdpService -Force -ErrorAction SilentlyContinue
    Stop-Service TermService -Force -ErrorAction SilentlyContinue
    try {
        (Get-Service TermService).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    } catch {
        Write-Host 'TermService did not stop within 30 s' -ForegroundColor Yellow
    }
    Start-Sleep -Seconds 3
    Start-Service TermService -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 10
    $st = (Get-Service TermService).Status
    if ($st -ne 'Running') {
        Write-Host "TermService is $st. STOP. On the physical console:" -ForegroundColor Red
        Write-Host '  reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f' -ForegroundColor Red
        Write-Host '  sc stop TermService' -ForegroundColor Red
        Write-Host '  sc start TermService' -ForegroundColor Red
        return $false
    }
    Write-Host 'TermService Running' -ForegroundColor Green
    return $true
}

function Get-ProtoSession {
    # " hydraproto#0   teacher   3  Active"  or, before logon,  " hydraproto#0   4  Conn"
    $line = & query.exe session 2>$null |
        Select-String 'hydraproto#' |
        Select-Object -First 1
    if (-not $line) { return $null }
    $tok = $line.ToString().Trim().TrimStart('>') -split '\s+'
    $idx = -1
    for ($i = 1; $i -lt $tok.Count; $i++) {
        if ($tok[$i] -match '^\d+$') { $idx = $i; break }
    }
    if ($idx -lt 0) { return $null }
    [pscustomobject]@{
        Name  = $tok[0]
        User  = if ($idx -eq 2) { $tok[1] } else { '' }
        Id    = [int]$tok[$idx]
        State = if ($idx + 1 -lt $tok.Count) { $tok[$idx + 1] } else { '' }
    }
}

function Set-ProtoCreds {
    param([string]$User = 'teacher')
    if (-not (Test-Path $ProtoKey)) {
        Write-Host 'listener key missing -- run rdsprov-register.ps1 -Register -Apply first' -ForegroundColor Red
        return
    }
    # -Register recreates this key EMPTY every time. Domain must be empty, not
    # the machine name.
    $s = Read-Host "password for $User" -AsSecureString
    $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($s)
    try {
        Set-ItemProperty $ProtoKey -Name Username -Value $User
        Set-ItemProperty $ProtoKey -Name Domain -Value ''
        Set-ItemProperty $ProtoKey -Name Password -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($b))
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b)
    }
    Get-ItemProperty $ProtoKey |
        Select-Object Username, Domain, @{ n = 'PwLen'; e = { $_.Password.Length } }
}

function Get-HydraIddPackage {
    # Precise on purpose: a loose oemNN pattern once deleted two system drivers.
    $e = (pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n"
    for ($i = 1; $i -lt $e.Count; $i++) {
        if ($e[$i] -match 'Original Name:\s+(iddseat|iddsample|iddremote)') {
            [pscustomobject]@{
                Published = ($e[$i - 1] -replace '.*:\s+', '')
                Original  = ($e[$i] -replace '.*:\s+', '')
            }
        }
    }
}

function Stop-ProtoRun {
    Remove-Item $TriggerFile -Force -ErrorAction SilentlyContinue
    $s = Get-ProtoSession
    if ($s) {
        Write-Host "logging off session $($s.Id) ($($s.Name))" -ForegroundColor Yellow
        & logoff.exe $s.Id
        Start-Sleep -Seconds 5
    }
    & query.exe session
}

function Start-ProtoRun {
    param(
        [Parameter(Mandatory)][string]$Label,
        [int]$Seconds = 90,
        [int]$Every = 2
    )

    # ---- refuse to start from a dirty state --------------------------------
    $k = Get-ItemProperty $ProtoKey -ErrorAction SilentlyContinue
    if (-not $k) {
        Write-Host 'listener key missing -- register first' -ForegroundColor Red
        return
    }
    if (-not $k.Username -or -not $k.Password) {
        Write-Host 'listener key has no credentials -- run Set-ProtoCreds' -ForegroundColor Red
        return
    }
    if (Get-ProtoSession) {
        Write-Host 'a hydraproto# session already exists -- run Stop-ProtoRun first' -ForegroundColor Red
        return
    }
    $existing = & query.exe session 2>$null |
        Select-String ("\s{0}\s+\d+\s" -f [regex]::Escape($k.Username))
    if ($existing) {
        Write-Host "$($k.Username) already has a session. A provider connection would be" -ForegroundColor Red
        Write-Host 'arbitrated onto it and the run would mean nothing. logoff <id> first.' -ForegroundColor Red
        $existing | ForEach-Object { Write-Host "  $_" }
        return
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir   = Join-Path $HydraRoot 'logs'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $out   = Join-Path $dir "mode4-guard-$Label-$stamp.txt"
    function Rec([string]$m, [string]$c = 'Gray') {
        Write-Host $m -ForegroundColor $c
        Add-Content -Path $out -Value $m
    }

    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $dllVer = (Get-Item "$env:SystemRoot\System32\termsrv.dll").VersionInfo
    $guard  = Join-Path $HydraRoot 'state\termsrv-guard.json'
    $flag   = Join-Path $HydraRoot 'state\termsrv-UNPATCHED'
    $sha    = (Get-FileHash "$env:SystemRoot\System32\termsrv.dll" -Algorithm SHA256).Hash
    $patched = 'no guard state file: stock, or patched by something else'
    if (Test-Path $guard) {
        $g = Get-Content $guard -Raw | ConvertFrom-Json
        $patched = if ($g.PatchedSha -eq $sha) { "PATCHED ($($g.Pattern))" } else { 'NOT PATCHED (hash differs from guard state)' }
    }
    if (Test-Path $flag) { $patched = 'NOT PATCHED (termsrv-UNPATCHED flag present)' }

    Rec "=== mode 4 retest, run $Label, $stamp ===" 'Cyan'
    Rec ("OS build      : {0}.{1} ({2})" -f $cv.CurrentBuild, $cv.UBR, $cv.DisplayVersion)
    Rec ("termsrv.dll   : {0}  {1}" -f $dllVer.FileVersionRaw, $patched)
    Rec ("ServiceDll    : {0}" -f (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters').ServiceDll)
    $pk = @(Get-HydraIddPackage)
    Rec ("IDD packages  : {0}" -f $(if ($pk.Count) { ($pk | ForEach-Object { "$($_.Published)<-$($_.Original)" }) -join ', ' } else { 'none staged' }))
    Rec ''

    # ---- proven order: stale trigger gone, credentials set, THEN restart ----
    Remove-Item $ProviderLog, $TriggerFile -Force -ErrorAction SilentlyContinue
    if (-not (Restart-HydraTs)) { Rec 'TermService did not come back; run aborted' 'Red'; return }

    Rec 'before trigger:'
    & query.exe session 2>$null | ForEach-Object { Rec "  $_" }
    Rec ''

    New-Item -ItemType Directory -Force -Path (Split-Path $TriggerFile) | Out-Null
    $t0 = Get-Date
    New-Item -ItemType File -Force -Path $TriggerFile | Out-Null
    Rec ("trigger written {0:HH:mm:ss.fff} -- the sample polls for it every 5 s" -f $t0) 'Cyan'

    $first = $null; $last = $null; $sawExplorer = $false; $devShown = $false
    $bestState = ''
    for ($t = $Every; $t -le $Seconds; $t += $Every) {
        Start-Sleep -Seconds $Every
        $el = [int]((Get-Date) - $t0).TotalSeconds
        $s  = Get-ProtoSession
        $con = & query.exe session 2>$null | Select-String '^[ >]console' | Select-Object -First 1
        $conState = if ($con) { (($con.ToString().Trim().TrimStart('>') -split '\s+') | Select-Object -Last 1) } else { '?' }
        if (-not $s) {
            Rec ("t+{0,3}s  no hydraproto# session          console={1}" -f $el, $conState)
            continue
        }
        if ($null -eq $first) { $first = $el }
        $last = $el
        if ($s.State -eq 'Active') { $bestState = 'Active' } elseif (-not $bestState) { $bestState = $s.State }
        $n = @((Get-CimInstance Win32_Process -Filter "SessionId=$($s.Id)" -ErrorAction SilentlyContinue).Name)
        $ex = $n -contains 'explorer.exe'
        if ($ex) { $sawExplorer = $true }
        Rec ("t+{0,3}s  id={1} {2,-6} user={3,-8} procs={4,-3} explorer={5,-5} dwm={6,-5} LogonUI={7,-5} console={8}" -f `
            $el, $s.Id, $s.State, $s.User, $n.Count, $ex, ($n -contains 'dwm.exe'), ($n -contains 'LogonUI.exe'), $conState)
        if (-not $devShown) {
            $devShown = $true
            Get-PnpDevice -ErrorAction SilentlyContinue |
                Where-Object { $_.InstanceId -match 'HYDRASEAT|REMOTEDISPLAYENUM' -and $_.Present } |
                ForEach-Object { Rec ("          devnode {0,-8} problem={1} {2}" -f $_.Status, $_.Problem, $_.InstanceId) 'DarkGray' }
        }
    }

    $alive = [bool](Get-ProtoSession)
    Rec ''
    Rec '--- summary ---' 'Cyan'
    if ($null -eq $first) {
        Rec 'session was never SEEN by the sampler. A 0.4 s session falls between samples:'
        Rec 'read provider.log and the LocalSessionManager events below before concluding.'
    } else {
        Rec ("first seen t+{0}s, last seen t+{1}s, best state {2}, explorer seen: {3}" -f $first, $last, $bestState, $sawExplorer)
    }
    Rec ("alive at t+{0}s: {1}" -f $Seconds, $alive) $(if ($alive) { 'Green' } else { 'Yellow' })

    Rec ''
    Rec '--- provider.log ---' 'Cyan'
    if (Test-Path $ProviderLog) { Get-Content $ProviderLog | ForEach-Object { Rec "  $_" } }
    else { Rec '  (no provider.log -- the provider did not log, or cannot write there)' 'Yellow' }

    # Only events from THIS run. Stale events cost three false readings in August.
    $evLogs = @(
        'Microsoft-Windows-TerminalServices-LocalSessionManager/Operational'
        'Microsoft-Windows-TerminalServices-RemoteConnectionManager/Operational'
        'Microsoft-Windows-DriverFrameworks-UserMode/Operational'
    )
    foreach ($l in $evLogs) {
        Rec ''
        Rec "--- $l (since trigger) ---" 'Cyan'
        $ev = Get-WinEvent -FilterHashtable @{ LogName = $l; StartTime = $t0 } -ErrorAction SilentlyContinue |
            Sort-Object TimeCreated
        if (-not $ev) { Rec '  (none)'; continue }
        foreach ($e in $ev) {
            $msg = if ($e.Message) { ($e.Message -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 2) -join ' | ' } else { '' }
            Rec ("  {0:HH:mm:ss.fff} +{1,6:N1}s id={2,-5} {3}" -f $e.TimeCreated, ($e.TimeCreated - $t0).TotalSeconds, $e.Id, $msg)
        }
    }
    Rec ''
    Rec '--- System log, warnings and errors (since trigger) ---' 'Cyan'
    $sys = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1, 2, 3; StartTime = $t0 } -ErrorAction SilentlyContinue |
        Sort-Object TimeCreated
    if (-not $sys) { Rec '  (none)' }
    foreach ($e in $sys) {
        $msg = if ($e.Message) { ($e.Message -split "`r?`n")[0] } else { '' }
        Rec ("  {0:HH:mm:ss.fff} {1} id={2} {3}" -f $e.TimeCreated, $e.ProviderName, $e.Id, $msg)
    }

    Rec ''
    Rec '--- after ---' 'Cyan'
    & query.exe session 2>$null | ForEach-Object { Rec "  $_" }
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match 'HYDRASEAT' } |
        ForEach-Object { Rec ("  devnode {0,-8} present={1} problem={2} {3}" -f $_.Status, $_.Present, $_.Problem, $_.InstanceId) }

    Write-Host ''
    Write-Host "saved: $out" -ForegroundColor Green
    if ($alive) {
        Write-Host 'The session is still up. Look at it, then run Stop-ProtoRun.' -ForegroundColor Green
    } else {
        Remove-Item $TriggerFile -Force -ErrorAction SilentlyContinue
    }
}
