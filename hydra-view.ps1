# hydra-view.ps1 -- start mode 3 (headless client) in one command.
#
# WHY THIS EXISTS
#   Mode 2 is one command. Mode 3 was two shells, a manual wait, and a rule about
#   ordering -- and getting the order wrong silently produced mirrors that showed
#   nothing, or two clients on one session, which wedges the RDP stack and costs
#   a reboot. None of that needed to be the user's problem.
#
#   The ordering is real, so this enforces it: stop anything conflicting, start
#   the service, start the client, WAIT until frames actually exist, then start
#   the mirrors.
#
# USAGE:
#   .\hydra-view.ps1
#   .\hydra-view.ps1 -NoWindow          # panel only, no view window
#   .\hydra-view.ps1 -Seat B -User teacher
#
# The client runs in ITS OWN WINDOW so its log stays readable -- that log is the
# only direct measure of whether frames are flowing.

param(
    [string]$Seat    = 'B',
    [string]$User    = 'teacher',
    [string]$Monitor = '\\.\DISPLAY2',
    [string]$ViewSize = '1600x900',
    [int]$Port       = 56789,
    [switch]$NoWindow,
    # Windows virtual desktop for the fullscreen view (0 = leave it here).
    #
    # Safe in THIS mode specifically: hydrardp has no window, so nothing can be
    # starved of compositing by sitting on an inactive desktop. That is not true
    # of mode 2, where parking the RDP client on another virtual desktop stops it
    # requesting updates and freezes the panel.
    [int]$Desktop = 0,
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$client = Join-Path $root 'dist\hydrardp.exe'
$mirror = Join-Path $root 'dist\mirror.exe'
foreach ($f in @($client, $mirror)) { if (-not (Test-Path $f)) { throw "missing: $f" } }

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

# --- 1. clear anything that would conflict --------------------------------
# Two clients on one session, or two producers on one pixel ring, wedges the
# stack. Cheaper to always clear than to explain the rule.
$stale = Get-Process mirror, hydrardp, sdl-freerdp, mstsc -ErrorAction SilentlyContinue
if ($stale) {
    Say "stopping $($stale.Count) conflicting process(es)" 'Yellow'
    $stale | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

# --- 2. service (it owns the shared rings) ---------------------------------
$svc = Get-Service Hydra -ErrorAction SilentlyContinue
if (-not $svc) { throw "Hydra service not installed -- run .\setup.ps1" }
if ($svc.Status -ne 'Running') { Say "starting Hydra service..."; Start-Service Hydra }
else                           { Say "Hydra service already running" }

# session_capture would write the SAME ring as the client. Only one producer.
Start-Sleep -Seconds 2
Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue

# gfx crashes before the first frame -- never inherit it from the parent shell.
Remove-Item Env:HYDRA_GFX -ErrorAction SilentlyContinue

# --- 3. client, in its own window ------------------------------------------
Say ""
Say "A window will open and ask for $User's password." 'Cyan'
Say "Type it there -- echo is off, so a typo shows as ERRCONNECT_LOGON_FAILURE." 'Cyan'
Say ""

Start-Process powershell -ArgumentList @(
    '-NoExit', '-Command',
    "cd '$root'; .\dist\hydrardp.exe $Seat $User"
)

# --- 4. WAIT for frames, rather than telling the user to watch for them ----
# The mirrors must not start against an empty ring: they come up at ~7 MB and
# display nothing, which looks exactly like a broken pipeline.
Say "waiting for the client to publish frames ..."
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $probe = & $mirror $Seat --probe 2 2>&1 | Out-String
    $ErrorActionPreference = $eap
    if ($probe -match 'seq starts at (\d+)') {
        if ([int]$Matches[1] -gt 0) { $ready = $true; break }
    }
}

if (-not $ready) {
    Say "no frames after $TimeoutSec s." 'Red'
    Say "Check the client window: a wrong password, or ERRCONNECT_ACTIVATION_TIMEOUT" 'Red'
    Say "which means the RDP stack is wedged and needs a reboot." 'Red'
    return
}
Say "frames are flowing" 'Green'

# --- 5. mirrors -------------------------------------------------------------
Say "starting the panel on $Monitor ..."
Start-Process $mirror -ArgumentList $Seat, $Monitor -WindowStyle Minimized

if (-not $NoWindow) {
    Say "starting the view window ($ViewSize, input forwarding on) ..."
    Start-Process $mirror -ArgumentList $Seat, '--window', $ViewSize, "$Port"

    if ($Desktop -gt 0) {
        try {
            if (-not (Get-Module -ListAvailable -Name VirtualDesktop)) {
                throw "module not installed"
            }
            Import-Module -Name VirtualDesktop -DisableNameChecking -ErrorAction Stop

            while ((Get-DesktopCount) -lt $Desktop) { New-Desktop | Out-Null }

            # Wait for the window: mirror opens borderless fullscreen a moment
            # after the process starts, and moving a handle that does not exist
            # yet silently does nothing.
            $hwnd = 0
            $deadlineW = (Get-Date).AddSeconds(10)
            while ((Get-Date) -lt $deadlineW) {
                $vp = Get-Process mirror -ErrorAction SilentlyContinue |
                      Where-Object { $_.MainWindowTitle -eq 'Hydra - seat view' } |
                      Select-Object -First 1
                if ($vp -and $vp.MainWindowHandle -ne 0) { $hwnd = $vp.MainWindowHandle; break }
                Start-Sleep -Milliseconds 300
            }

            if ($hwnd -ne 0) {
                Move-Window -Desktop (Get-Desktop ($Desktop - 1)) -Hwnd $hwnd | Out-Null
                Say "view window moved to virtual desktop $Desktop" 'Green'
                Say "  switch with Win+Ctrl+Left / Win+Ctrl+Right" 'DarkGray'
            } else {
                Say "  view window did not appear in time; left on this desktop" 'Yellow'
            }
        } catch {
            Say "virtual desktop $Desktop unavailable -- run:" 'Yellow'
            Say "  Install-Module VirtualDesktop -Scope CurrentUser -Force" 'Yellow'
        }
    }
}

Start-Sleep -Seconds 3
$m = @(Get-Process mirror -ErrorAction SilentlyContinue)
foreach ($p in $m) {
    $mb = [math]::Round($p.WorkingSet64 / 1MB, 1)
    $what = if ($p.MainWindowTitle) { $p.MainWindowTitle } else { 'panel' }
    $col = if ($mb -gt 40) { 'Green' } else { 'Yellow' }
    Say ("  {0,-18} {1,6} MB" -f $what, $mb) $col
}
Say ""
Say "Under 40 MB means a mirror started before frames existed -- rerun this script." 'DarkGray'
Say "Known in this mode: video is glitchy (no codec), and the cursor renders but" 'DarkGray'
Say "does not track (RDP sends no pointer position to a client that sends no input)." 'DarkGray'
