#requires -Version 5.1
#requires -RunAsAdministrator
<#
    hydra-trace.ps1 -- capture what the RD stack and PnP actually do during a
                       connection attempt.

    WHY THIS EXISTS

    Six hypotheses have failed in a row. The facts are sharp but the cause is
    not visible from outside:

      - the provider reaches ConnectNotify, then PreDisconnect reason=17 at
        +4ms to +320ms, and the session dies
      - with the driver package UNSTAGED the same run reaches
        NotifyCommandProcessCreated and the session survives (clean A/B)
      - setupapi.dev.log records NO install attempt on these runs at all, yet
        `pnputil /add-driver /install` installs the same package successfully
        onto SWD\RemoteDisplayEnum devnodes

    So something between "the stack asked for the hardware id" and "PnP would
    have been asked to install" is failing silently. Nothing user-visible logs
    it. This is the same position as yesterday morning's /gfx crash, and what
    broke that was building an instrument instead of guessing again.

    WHAT IT CAPTURES

    ETW providers, by GUID, for the whole connection attempt:

      Microsoft-Windows-TerminalServices-RemoteConnectionManager
      Microsoft-Windows-TerminalServices-LocalSessionManager
      Microsoft-Windows-TerminalServices-SessionBroker-Client
      Microsoft-Windows-Kernel-PnP
      Microsoft-Windows-UserPnp
      Microsoft-Windows-DriverFrameworks-UserMode   <- the UMDF host itself

    The last one is the interesting one: it is where WUDFHost reports why it
    would not start, and nothing else surfaces that.

    USAGE

        .\hydra-trace.ps1 -Start
        ... trigger a connection ...
        .\hydra-trace.ps1 -Stop

    Or all in one, which starts the trace, triggers, waits and stops:

        .\hydra-trace.ps1 -Run

    Output lands in C:\ProgramData\Hydra\trace\ as an .etl plus a decoded
    .txt. Read the .txt around the ConnectNotify timestamp from provider.log.

    NOTE ON UMDF VERBOSITY

    WUDFHost logs much more when its diagnostic registry values are raised.
    -EnableUmdfVerbose sets them; -DisableUmdfVerbose puts them back. They
    persist across reboots, so turn them off when finished.
#>
[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Start')] [switch] $Start,
    [Parameter(ParameterSetName = 'Stop')]  [switch] $Stop,
    [Parameter(ParameterSetName = 'Run')]   [switch] $Run,
    [Parameter(ParameterSetName = 'Verbose')][switch] $EnableUmdfVerbose,
    [Parameter(ParameterSetName = 'Quiet')] [switch] $DisableUmdfVerbose,

    [string] $OutDir      = 'C:\ProgramData\Hydra\trace',
    [string] $SessionName = 'HydraRds',
    [int]    $WaitSec     = 30
)

$ErrorActionPreference = 'Stop'

# PS 7.4 made native-command stderr honour ErrorActionPreference. Several tools
# here write PROGRESS to stderr -- hydractl's 'not reachable' while it waits,
# mirror's 'pixel transport opened' -- and 2>&1 under 'Stop' turned those
# SUCCESS lines into terminating errors. This broke hydra-start.ps1 on 2026-08-21.
$PSNativeCommandUseErrorActionPreference = $false
# Providers by GUID -- names are unreliable across builds, GUIDs are not.
$providers = @(
    @{ Name = 'TerminalServices-RemoteConnectionManager'; Guid = '{C76BAA63-AE81-421C-B425-340B4B24157F}' }
    @{ Name = 'TerminalServices-LocalSessionManager';     Guid = '{5D896912-022D-40AA-A3A8-4FA5515C76D7}' }
    @{ Name = 'TerminalServices-RdpCoreTS';               Guid = '{1139C61B-B549-4251-8ED3-27250A1EDEC8}' }
    @{ Name = 'Kernel-PnP';                               Guid = '{9C205A39-1250-487D-ABD7-E831C6290539}' }
    @{ Name = 'UserPnp';                                  Guid = '{96F4A050-7E31-453C-88BE-9634F4E02139}' }
    @{ Name = 'DriverFrameworks-UserMode';                Guid = '{2E35AAEB-857F-4BEB-A418-2E6C0E54D988}' }
)

$umdfKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\WUDF'

function Enable-UmdfVerbose {
    New-Item -Path $umdfKey -Force | Out-Null
    # 4 = verbose. HostProcessDbgBreakOnStart stays 0 -- we want logs, not a
    # debugger break that would hang the whole install.
    Set-ItemProperty $umdfKey -Name 'LogLevel'            -Value 4 -Type DWord
    Set-ItemProperty $umdfKey -Name 'HostProcessDbgBreakOnStart' -Value 0 -Type DWord
    Write-Host "UMDF verbose logging ON (LogLevel=4)" -ForegroundColor Green
    Write-Host "  remember: .\hydra-trace.ps1 -DisableUmdfVerbose when finished" -ForegroundColor DarkGray
}

function Disable-UmdfVerbose {
    if (Test-Path $umdfKey) {
        Remove-ItemProperty $umdfKey -Name 'LogLevel' -ErrorAction SilentlyContinue
        Write-Host "UMDF verbose logging OFF" -ForegroundColor Green
    }
}

function Start-Trace {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    # Clear any stale session from an interrupted run.
    & logman stop $SessionName -ets 2>&1 | Out-Null

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $etl   = Join-Path $OutDir "$SessionName-$stamp.etl"
    Set-Content -Path (Join-Path $OutDir 'last.txt') -Value $etl -Encoding UTF8

    # -ets creates and starts in one step, no data-collector-set plumbing.
    # Buffers sized generously: PnP is chatty and a dropped event is the one
    # that would have mattered.
    $out = & logman create trace $SessionName -ets -o $etl -nb 32 256 -bs 1024 -mode Circular -max 256 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
        throw "logman create failed"
    }

    foreach ($p in $providers) {
        # 0xFFFFFFFFFFFFFFFF = all keywords, 5 = verbose level.
        $r = & logman update trace $SessionName -ets -p $p.Guid 0xffffffffffffffff 5 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host ("  + {0}" -f $p.Name) -ForegroundColor DarkGray
        } else {
            Write-Host ("  ! {0} not enabled" -f $p.Name) -ForegroundColor Yellow
        }
    }

    Write-Host ""
    Write-Host "tracing to $etl" -ForegroundColor Green
    Write-Host "trigger the connection now, then: .\hydra-trace.ps1 -Stop" -ForegroundColor Cyan
    return $etl
}

function Stop-Trace {
    $lastFile = Join-Path $OutDir 'last.txt'
    if (-not (Test-Path $lastFile)) { throw "no trace in progress (no $lastFile)" }
    $etl = (Get-Content $lastFile -Raw).Trim()

    & logman stop $SessionName -ets 2>&1 | Out-Null
    Write-Host "stopped." -ForegroundColor Green

    if (-not (Test-Path $etl)) { throw "etl not found: $etl" }
    Write-Host ("  {0:N0} bytes  {1}" -f (Get-Item $etl).Length, $etl)

    # Decode. tracerpt is present on every Windows install; netsh/wpa are not.
    $txt = [IO.Path]::ChangeExtension($etl, '.txt')
    $xml = [IO.Path]::ChangeExtension($etl, '.xml')
    Write-Host "decoding (this takes a minute) ..." -ForegroundColor Cyan
    & tracerpt $etl -o $xml -summary ([IO.Path]::ChangeExtension($etl,'.summary.txt')) -f XML -y 2>&1 | Out-Null

    if (Test-Path $xml) {
        # XML is unreadable as-is; flatten to timestamped one-liners.
        Write-Host "flattening to $txt ..." -ForegroundColor Cyan
        try {
            [xml]$doc = Get-Content $xml -Raw
            $lines = foreach ($e in $doc.Events.Event) {
                $t   = $e.System.TimeCreated.SystemTime
                $prov= $e.System.Provider.Name
                if (-not $prov) { $prov = $e.System.Provider.Guid }
                $id  = $e.System.EventID
                $data = ($e.EventData.Data | ForEach-Object {
                            if ($_.Name) { "$($_.Name)=$($_.'#text')" } else { $_.'#text' }
                         }) -join ' '
                "{0}  [{1}]  id={2}  {3}" -f $t, $prov, $id, $data
            }
            $lines | Set-Content -Path $txt -Encoding UTF8
            Write-Host ("  {0:N0} events -> {1}" -f $lines.Count, $txt) -ForegroundColor Green
        } catch {
            Write-Host "  XML flatten failed: $_" -ForegroundColor Yellow
            Write-Host "  read the raw XML instead: $xml" -ForegroundColor Yellow
        }
    }

    Write-Host ""
    Write-Host "WHAT TO LOOK FOR:" -ForegroundColor Cyan
    Write-Host "  1. find the ConnectNotify timestamp in C:\ProgramData\Hydra\provider.log"
    Write-Host "  2. in the trace, read the 200ms AROUND it"
    Write-Host "  3. the question is what happens between GetHardwareId and PreDisconnect --"
    Write-Host "     a PnP device-arrival that fails, a UMDF host that will not start,"
    Write-Host "     or an RCM error that never reaches any user-visible log"
    Write-Host ""
    Write-Host "  Select-String -Path '$txt' -Pattern 'HydraSeat|RemoteDisplay|WUDF|Idd' -Context 2,2" -ForegroundColor DarkGray
}

function Invoke-Run {
    Start-Trace | Out-Null

    Write-Host ""
    Write-Host "triggering ..." -ForegroundColor Cyan
    Remove-Item 'C:\ProgramData\Hydra\provider.log','C:\TestProtocol\createconnection.txt' -Force -ErrorAction SilentlyContinue
    Stop-Service TermService -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    Start-Service TermService -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 10
    New-Item -ItemType Directory -Force 'C:\TestProtocol' | Out-Null
    New-Item -ItemType File -Force 'C:\TestProtocol\createconnection.txt' | Out-Null
    Start-Sleep -Seconds $WaitSec

    Write-Host ""
    & query session
    Write-Host ""
    Write-Host "--- provider.log ---" -ForegroundColor Cyan
    Get-Content 'C:\ProgramData\Hydra\provider.log' -ErrorAction SilentlyContinue
    Write-Host ""

    Stop-Trace
}

switch ($PSCmdlet.ParameterSetName) {
    'Start'   { Start-Trace | Out-Null }
    'Stop'    { Stop-Trace }
    'Verbose' { Enable-UmdfVerbose }
    'Quiet'   { Disable-UmdfVerbose }
    default   { Invoke-Run }
}
