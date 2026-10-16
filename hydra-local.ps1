# Experimental seat-scoped local client launcher. Default action is a dry plan.
# Existing Hydra/Interception/concurrent-session setup is a prerequisite.
[CmdletBinding(DefaultParameterSetName='Plan')]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_-]{1,32}$')][string]$Seat,
    [Parameter(ParameterSetName='Start', Mandatory=$true)][switch]$Start,
    [Parameter(ParameterSetName='Stop', Mandatory=$true)][switch]$StopClient,
    [Parameter(ParameterSetName='Plan')][switch]$Plan,
    [string]$Root = $PSScriptRoot,
    [Parameter(ParameterSetName='Plan')][string]$Config
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'local\HydraLocal.psm1') -Force
$Root = [IO.Path]::GetFullPath($Root)
$dist = Join-Path $Root 'dist'
$client = Join-Path $dist 'freerdp\sdl-freerdp.exe'
$control = Join-Path $dist 'hydractl.exe'
$stateDirectory = Join-Path $Root 'state\local-clients'
$statePath = Join-Path $stateDirectory "$Seat.json"
$consoleSession = (Get-Process -Id $PID).SessionId

function Invoke-ReadCommand([string]$Path, [string[]]$Arguments, [int[]]$AcceptedExitCodes = @(0)) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing prerequisite: $Path" }
    $old = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $text = (& $Path @Arguments 2>&1 | Out-String)
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    if ($code -notin $AcceptedExitCodes) { throw "$Path exited $code`: $text" }
    return $text
}

function Get-ClientRecord($Process) {
    # Materialize a handle so later operations refer to this process, not a reused PID.
    $null = $Process.Handle
    [pscustomobject]@{
        Id = $Process.Id; StartUtcTicks = [string]$Process.StartTime.ToUniversalTime().Ticks
        Path = $Process.MainModule.FileName; ConsoleSession = $Process.SessionId
    }
}

# Serialize operations on this seat only. Other seats have independent locks.
$sha = [Security.Cryptography.SHA256]::Create()
try { $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Root.ToLowerInvariant()))).Replace('-', '') }
finally { $sha.Dispose() }
$mutex = New-Object Threading.Mutex($false, "Local\HydraLocal_$($key.Substring(0,16))_$Seat")
$locked = $false
try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw "Another operation is in progress for seat $Seat." }
    $saved = $null
    if (Test-Path -LiteralPath $statePath) { $saved = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json }
    if ($null -ne $saved) {
        $existing = Get-Process -Id $saved.Id -ErrorAction SilentlyContinue
        if ($null -ne $existing) {
            try {
                $record = Get-ClientRecord $existing
                if (-not (Test-HydraClientIdentity $saved $record $client $consoleSession)) {
                    throw "Stale or mismatched process record for seat $Seat; no process was stopped."
                }
                if ($StopClient) {
                    # Stops only the exact launcher-owned client; does not log off the user.
                    $existing.Kill()
                    $existing.WaitForExit()
                    Remove-Item -LiteralPath $statePath
                    "Seat $Seat client closed. Its Windows session remains for reconnection."
                    return
                }
                if ($Start) { throw "Seat $Seat already has a live client (PID $($saved.Id))." }
            } finally { $existing.Dispose() }
        } elseif ($StopClient) {
            Remove-Item -LiteralPath $statePath
            "Seat $Seat client has already exited."
            return
        }
    }
    if ($StopClient) { "No tracked client for seat $Seat."; return }
    if (-not $Config) { $Config = Join-Path $dist 'seats.toml' }
    $configuration = Invoke-ReadCommand $control @('config', $Config) | ConvertFrom-Json
    $windowsListing = Invoke-ReadCommand (Join-Path $dist 'clip_console.exe') @()
    # FreeRDP 3.32.1 on Windows returns -1 for this successful informational
    # command. The structured listing is still validated by the planner below.
    $clientListing = Invoke-ReadCommand $client @('/list:monitor') @(0, -1)
    $launch = New-HydraLocalPlan $configuration $Seat $windowsListing $clientListing
    if (-not $Start) { $launch; return }

    # Check the actual running configuration, not merely a possibly edited file.
    $service = Get-CimInstance Win32_Service -Filter "Name='Hydra'"
    if ($null -eq $service -or $service.State -ne 'Running') {
        throw 'Hydra must already be configured and running. This launcher does not install/start input drivers or services.'
    }
    $serviceExecutable = $service.PathName.Trim().Trim('"')
    if ($serviceExecutable -ne (Join-Path $dist 'hydrad.exe')) { throw 'The running Hydra service uses a different installation.' }
    $live = Invoke-ReadCommand $control @('live-config') | ConvertFrom-Json
    if (($live | ConvertTo-Json -Depth 6 -Compress) -cne ($configuration | ConvertTo-Json -Depth 6 -Compress)) {
        throw 'The file differs from the live seat configuration. Resolve it before starting another client.'
    }
    $status = Invoke-ReadCommand $control @('status')
    if ($status -notmatch '(?m)^\s+router: running\b') { throw 'The input router is not running.' }
    $account = Get-LocalUser -Name $launch.User
    if (-not $account.Enabled) { throw 'The selected local Windows account is disabled.' }
    $consoleUser = (Get-CimInstance Win32_ComputerSystem).UserName
    if ($consoleUser -and ($consoleUser -split '\\')[-1] -eq $launch.User) {
        throw 'An extra seat must not reconnect the console user through RDP.'
    }
    if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server').fDenyTSConnections -ne 0) {
        throw 'Concurrent local RDP sessions must be configured first; Remote Desktop is disabled.'
    }
    # No password is saved or placed in argv. FreeRDP owns the credential prompt.
    New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $client
    $info.Arguments = $launch.CommandLine
    $info.UseShellExecute = $false
    $info.WorkingDirectory = Split-Path $client -Parent
    $process = [Diagnostics.Process]::Start($info)
    try {
        $record = Get-ClientRecord $process
        $record | Add-Member NoteProperty Seat $Seat
        [IO.File]::WriteAllText($statePath, ($record | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    } catch {
        if (-not $process.HasExited) { $process.Kill() }
        throw
    } finally { $process.Dispose() }
    "Started the local client for $Seat on $($launch.Display). Complete its login prompt."
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
