#requires -Version 5.1
#requires -RunAsAdministrator
<#
    rdsprov-register.ps1 -- register / unregister the RDS protocol provider,
                            with an undo that works even after it breaks RDP.

    WHAT THIS TOUCHES, and why it is dangerous

    Registering a protocol provider makes termsrv load your DLL into its
    svchost when the service starts. A bad provider takes Terminal Services
    down with it -- which stops seat B, and can stop RDP entirely, including
    any RDP you might have been planning to use to fix it.

    So this script does three things before it changes anything:

      1. exports HKLM\...\Terminal Server\WinStations to a .reg file
      2. writes a plain-text emergency card next to it, with the exact
         recovery commands, readable from a recovery console or another PC
      3. refuses to proceed if TermService is not currently healthy

    USAGE

        .\rdsprov-register.ps1 -Status          # what is registered now
        .\rdsprov-register.ps1 -Register        # dry run: prints the plan
        .\rdsprov-register.ps1 -Register -Apply
        .\rdsprov-register.ps1 -Unregister -Apply

    THE UNDO, if this shell is gone and RDP is dead:

        reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
        sc stop TermService
        sc start TermService

    That is the whole recovery. Deleting the key is enough -- termsrv stops
    loading the DLL on the next start. The COM registration is harmless on its
    own; nothing loads it unless the WinStations key points at it.
#>
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Status')]   [switch] $Status,
    [Parameter(ParameterSetName = 'Register')] [switch] $Register,
    [Parameter(ParameterSetName = 'Unregister')][switch] $Unregister,

    [Parameter(ParameterSetName = 'Register')]
    [Parameter(ParameterSetName = 'Unregister')]
    [switch] $Apply,

    [string] $ListenerName = 'HydraProto',
    [string] $Clsid        = '{23b3ed19-0299-45bd-b235-0c0c9bab40a4}',
    [string] $Dll          = 'C:\Programs\rdsprov\Sample\x64\Release\TestProtocol_Ext.dll',
    [string] $BackupDir    = 'C:\ProgramData\Hydra\rdsprov-backup'
)

$ErrorActionPreference = 'Stop'

$WinStations = 'HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations'
$RegPath     = "$WinStations\$ListenerName"
$RegPathNative = "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\$ListenerName"

function Show-Status {
    Write-Host "listeners:" -ForegroundColor Cyan
    Get-ChildItem $WinStations | ForEach-Object {
        $lp = (Get-ItemProperty $_.PSPath -Name LoadableProtocol_Object -ErrorAction SilentlyContinue).LoadableProtocol_Object
        $en = (Get-ItemProperty $_.PSPath -Name fEnableWinStation -ErrorAction SilentlyContinue).fEnableWinStation
        "  {0,-16} enabled={1,-4} LoadableProtocol_Object={2}" -f $_.PSChildName, $en, $lp
    }
    Write-Host ""
    $svc = Get-Service TermService
    Write-Host ("TermService: {0} ({1})" -f $svc.Status, $svc.StartType) -ForegroundColor Cyan
    Write-Host ""
    $com = "Registry::HKCR\CLSID\$Clsid"
    if (Test-Path $com) {
        $inproc = (Get-ItemProperty "$com\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
        Write-Host "COM class registered -> $inproc" -ForegroundColor Cyan
    } else {
        Write-Host "COM class NOT registered" -ForegroundColor DarkGray
    }
}

function New-EmergencyCard([string]$backupFile) {
    $card = Join-Path $BackupDir 'EMERGENCY-UNDO.txt'
    @(
        "HYDRA RDS PROTOCOL PROVIDER -- EMERGENCY UNDO"
        "written $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        ""
        "If Terminal Services will not start, or RDP is dead, or seat B is gone:"
        ""
        "  1. Open an ADMIN command prompt on the physical console."
        "     (Not RDP. RDP is the thing that is broken.)"
        ""
        "  2. Delete the listener key:"
        ""
        "     reg delete `"$RegPathNative`" /f"
        ""
        "  3. Restart the service:"
        ""
        "     sc stop TermService"
        "     sc start TermService"
        ""
        "  4. Check it came back:"
        ""
        "     sc query TermService"
        "     query session"
        ""
        "That is sufficient. Deleting the key stops termsrv loading the DLL."
        "The COM registration alone is inert -- nothing loads it without the key."
        ""
        "If the machine will not boot far enough for that, the full WinStations"
        "subtree was exported before any change was made:"
        ""
        "     $backupFile"
        ""
        "  reg import `"$backupFile`""
        ""
        "To also remove the COM registration (optional, not required):"
        ""
        "     regsvr32 /u `"$Dll`""
        ""
        "Listener name : $ListenerName"
        "Manager CLSID : $Clsid"
        "Provider DLL  : $Dll"
        ""
        "Microsoft's own RDP manager, for comparison, lives under RDP-Tcp as"
        "LoadableProtocol_Object = {5828227c-20cf-4408-b73f-73ab70b8849f}."
        "Do not touch that one."
    ) | Set-Content -Path $card -Encoding UTF8
    return $card
}

# --- status ----------------------------------------------------------------

if ($PSCmdlet.ParameterSetName -eq 'Status' -or $Status) {
    Show-Status
    return
}

# --- unregister ------------------------------------------------------------

if ($Unregister) {
    Write-Host "plan:" -ForegroundColor Cyan
    Write-Host "  remove  $RegPathNative"
    Write-Host "  regsvr32 /u $Dll"
    Write-Host "  restart TermService"
    if (-not $Apply) {
        Write-Host ""
        Write-Host "nothing changed. re-run with -Apply." -ForegroundColor Yellow
        return
    }

    if (Test-Path $RegPath) {
        Remove-Item $RegPath -Recurse -Force
        Write-Host "listener key removed" -ForegroundColor Green
    } else {
        Write-Host "listener key was not present" -ForegroundColor DarkGray
    }

    if (Test-Path $Dll) {
        & regsvr32.exe /u /s $Dll
        Write-Host "COM class unregistered" -ForegroundColor Green
    }

    Write-Host "restarting TermService ..."
    Restart-Service TermService -Force
    Start-Sleep -Seconds 5
    Show-Status
    return
}

# --- register --------------------------------------------------------------

if (-not (Test-Path $Dll)) { throw "provider DLL not found: $Dll" }

$svc = Get-Service TermService
if ($svc.Status -ne 'Running') {
    throw "TermService is $($svc.Status). Fix that before registering anything."
}

Write-Host "plan:" -ForegroundColor Cyan
Write-Host "  export   $WinStations"
Write-Host "  regsvr32 $Dll"
Write-Host "  create   $RegPathNative"
Write-Host "             LoadableProtocol_Object = $Clsid   (REG_SZ)"
Write-Host "             fEnableWinStation       = 1        (REG_DWORD)"
Write-Host "  restart  TermService"
Write-Host ""
Write-Host "  If TermService does not come back, run this on the PHYSICAL console:" -ForegroundColor Yellow
Write-Host "    reg delete `"$RegPathNative`" /f" -ForegroundColor Yellow
Write-Host "    sc stop TermService" -ForegroundColor Yellow
Write-Host "    sc start TermService" -ForegroundColor Yellow

if (-not $Apply) {
    Write-Host ""
    Write-Host "nothing changed. re-run with -Apply." -ForegroundColor Yellow
    Write-Host "Do this on a morning with no lessons, at the physical console." -ForegroundColor Yellow
    return
}

New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $BackupDir "WinStations-$stamp.reg"
& reg.exe export "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations" $backup /y | Out-Null
if (-not (Test-Path $backup)) { throw "registry export failed -- refusing to continue" }
Write-Host "exported  $backup" -ForegroundColor Green

$card = New-EmergencyCard $backup
Write-Host "undo card $card" -ForegroundColor Green
Write-Host ""
Write-Host "READ THAT CARD NOW, or print it. If this goes wrong you will not" -ForegroundColor Yellow
Write-Host "be able to open it over RDP." -ForegroundColor Yellow
Write-Host ""

& regsvr32.exe /s $Dll
Write-Host "COM class registered" -ForegroundColor Green

New-Item -Path $RegPath -Force | Out-Null
New-ItemProperty -Path $RegPath -Name 'LoadableProtocol_Object' -Value $Clsid -PropertyType String -Force | Out-Null
New-ItemProperty -Path $RegPath -Name 'fEnableWinStation'       -Value 1      -PropertyType DWord  -Force | Out-Null
Write-Host "listener key created" -ForegroundColor Green

Write-Host ""
Write-Host "restarting TermService -- this is the moment of truth ..." -ForegroundColor Cyan
try {
    Stop-Service TermService -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    Start-Service TermService -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 5
    if ((Get-Service TermService).Status -ne 'Running') { throw 'TermService did not come back' }
} catch {
    Write-Host ""
    Write-Host "TermService failed to restart: $_" -ForegroundColor Red
    Write-Host "ROLLING BACK automatically ..." -ForegroundColor Red
    Remove-Item $RegPath -Recurse -Force -ErrorAction SilentlyContinue
    Start-Service TermService -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 5
    Show-Status
    Write-Host ""
    Write-Host "If TermService is still not Running, use the card:" -ForegroundColor Red
    Write-Host "  $card" -ForegroundColor Red
    return
}

Start-Sleep -Seconds 5
Show-Status

Write-Host ""
Write-Host "if TermService is Running and RDP-Tcp still works, the provider loaded." -ForegroundColor Green
Write-Host ""
Write-Host "next, trigger the sample's connection path:" -ForegroundColor Cyan
Write-Host "  New-Item -ItemType Directory -Force C:\TestProtocol | Out-Null"
Write-Host "  New-Item -ItemType File -Force C:\TestProtocol\createconnection.txt | Out-Null"
Write-Host "  Start-Sleep 10; query session"
Write-Host ""
Write-Host "a session with protocol name 'TL-Ext' means it worked." -ForegroundColor DarkGray
Write-Host "undo any time:  .\rdsprov-register.ps1 -Unregister -Apply" -ForegroundColor DarkGray
