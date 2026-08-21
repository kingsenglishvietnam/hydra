# hydra6.ps1 -- mode 6 cold start, one command.
#
# Mode 6: the RDP client runs FULLSCREEN on a virtual display, so it can never
# be covered, minimised, or stranded on an inactive virtual desktop. That is the
# whole family of failures that shaped modes 1-3 -- see hydra-start.ps1:345,
# where all three routes are documented as measured.
#
#   ROOT\DISPLAY\0000   virtual display (VDD), permanent via devcon
#     -> sdl-freerdp    fullscreen on it, never touched, always composited
#        -> seat B session
#           -> session_capture (DDA) -> shared ring
#              -> mirror B \\.\DISPLAY2      the student's panel
#              -> mirror B --window 56789    your control window, on VD2
#
# The student's wireless keyboard and mouse are routed by seat_router ->
# agent:B and need NO window and NO focus. The control window is only for
# driving the seat from the console.
#
# USAGE (elevated, from the Hydra Shell)
#   .\hydra6.ps1
#   .\hydra6.ps1 -NoControl        # student panel only, no console window
#   .\hydra6.ps1 -Desktop 0        # leave the control window on this desktop
#   .\hydra6.ps1 -Stop             # tear everything down cleanly

param(
    [int]$Desktop   = 2,        # virtual desktop for the control window; 0 = don't move
    [switch]$NoControl,
    [switch]$Stop,
    [string]$Seat   = 'B',
    [string]$User   = 'teacher',
    [string]$Panel  = '\\.\DISPLAY2',
    [int]$Port      = 56789,    # router's agent port -- the control window needs
                                # this or it renders but forwards nothing
    [string]$Root   = 'C:\Programs\hydra'
)

$ErrorActionPreference = 'Continue'
# PS 7.4 made native-command stderr honour ErrorActionPreference. Several tools
# here write PROGRESS to stderr -- hydractl's 'not reachable' while it waits,
# mirror's 'pixel transport opened' -- and 2>&1 under 'Stop' turned those
# SUCCESS lines into terminating errors. This broke hydra-start.ps1 on 2026-08-21.
$PSNativeCommandUseErrorActionPreference = $false
Set-Location $Root

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

# ---------------------------------------------------------------- stop -----
function Stop-Everything {
    Say "stopping ..." Cyan
    Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay -EA SilentlyContinue | Stop-Process -Force
    Stop-Service Hydra -EA SilentlyContinue
    Start-Sleep 2
    $t = query session | Select-String $User
    if ($t) {
        $sid = ($t.ToString().Trim() -split '\s+' | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1)
        if ($sid) {
            # A disconnected session is held by RDP-Wrapper and only a reboot
            # clears it -- log it off properly.
            Say "  logging off $User session $sid" Yellow
            logoff $sid 2>$null
        }
    }
    Say "stopped." Green
}

if ($Stop) { Stop-Everything; return }

Stop-Everything
Say ""

# ------------------------------------------------------- virtual display ---
# Installed root-enumerated with devcon, so it persists across reboots:
#   devcon install C:\Programs\vdd\VirtualDisplayDriver\MttVDD.inf "Root\MttVDD"
# devgen was the wrong tool -- it makes a SOFTWARE device owned by the creating
# process, which became a phantom the moment devgen.exe exited.
$vdd = Get-PnpDevice -EA SilentlyContinue |
       Where-Object { $_.FriendlyName -match 'Virtual Display' -and $_.Status -eq 'OK' } |
       Select-Object -First 1
if (-not $vdd) {
    Say "NO VIRTUAL DISPLAY. Mode 6 needs one. Reinstall it with:" Red
    Say "  devcon install C:\Programs\vdd\VirtualDisplayDriver\MttVDD.inf `"Root\MttVDD`"" Red
    Say "  (devcon.exe is under Windows Kits\10\Tools\10.0.26100.0\x64)" Red
    return
}
Say "virtual display: $($vdd.InstanceId)" Green

# ---------------------------------------------------------- preconditions --
$svc = & sc.exe qc TermService | Out-String
if ($svc -notmatch 'WIN32_OWN_PROCESS') {
    Say "TermService is not type= own -- RDP-Wrapper's ServiceDll will not load," Red
    Say "you get ONE session, and the symptom is ERRCONNECT_ACTIVATION_TIMEOUT." Red
    Say "  sc.exe config TermService type= own   (then reboot)" Red
    return
}

# ------------------------------------------------------------- service -----
# FIRST. It creates the shared sections and binds the router's listener. A
# mirror started before this gets refused and STAYS refused -- that is what
# made input forwarding look broken.
Say "starting Hydra service ..." Cyan
Start-Service Hydra
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep 1
    if ((& "$Root\dist\hydractl.exe" status 2>&1 | Out-String) -notmatch 'not reachable') { break }
}
& "$Root\dist\hydractl.exe" status 2>&1 | Out-String | Write-Host -ForegroundColor DarkGray

# -------------------------------------------------------- monitor index ----
# FreeRDP's own enumeration, 1-based, and NOT the Windows DISPLAY number. It
# shifts between boots -- it has been 3, 4 and 5 within one day. Never hardcode.
$mon = & "$Root\dist\freerdp\sdl-freerdp.exe" /list:monitor 2>&1 | Out-String
$idx = $null
foreach ($line in ($mon -split "`r?`n")) {
    if ($line -match '\[(\d+)\]\s+\[VDD') { $idx = $Matches[1]; break }
}
if (-not $idx) {
    Say "could not find the VDD monitor in /list:monitor:" Red
    Write-Host $mon
    return
}
Say "VDD is FreeRDP monitor $idx" Green

# ------------------------------------------------------------- client ------
# /sound            REQUIRED. It creates the seat session's audio ENDPOINT.
#                   Without it abcap fails 0x80070490 forever and there is no
#                   sound -- audio_bridge carries the audio, but the endpoint
#                   has to exist for it to capture from.
# -suppress-output  stops the client suppressing frames when it thinks it is
#                   not visible.
# /scale:140        session DPI, so the seat's UI is readable while the display
#                   stays 1:1 and mirror does no stretching.
# /d:               puts login focus on the password field, not the domain one.
Say ""
Say "starting the client on the virtual display -- LOG IN AS $User" Cyan
Say "  (echo is off; a typo shows as ERRCONNECT_LOGON_FAILURE)" DarkGray
Start-Process "$Root\dist\freerdp\sdl-freerdp.exe" -ArgumentList @(
    "/v:127.0.0.2", "/u:$User", "/d:", "/cert:ignore",
    "/sound", "-suppress-output", "/scale:140", "+auto-reconnect",
    "/f", "/monitors:$idx"
)

Say "waiting for the seat session ..." Yellow
$ok = $false
for ($i = 0; $i -lt 90; $i++) {
    Start-Sleep 2
    if ((& query session | Out-String) -match "$User\s+\d+\s+Active") { $ok = $true; break }
}
if (-not $ok) { Say "no $User session after 3 minutes -- stopping here." Red; return }
Say "seat session up." Green

# ------------------------------------------------------------- mirrors -----
# Wait for frames before starting any mirror. Against an empty ring a mirror
# sits blank and never recovers on its own.
Say "waiting for frames ..." Yellow
$seen = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep 2
    $shm = & "$Root\hydra-shm.ps1" 6>&1 | Out-String
    if ($shm -match 'ready=1' -and $shm -match 'seq=([1-9]\d*)') { $seen = $true; break }
}
if (-not $seen) { Say "  no frames yet -- starting the panel anyway, it may be blank" Yellow }
else { Say "frames flowing." Green }

Say "panel -> $Panel" Cyan
Start-Process "$Root\dist\mirror.exe" -ArgumentList $Seat, $Panel

if (-not $NoControl) {
    Start-Sleep 2
    Say "control window (port $Port) -- hover it to drive seat $Seat" Cyan
    Start-Process "$Root\dist\mirror.exe" -ArgumentList $Seat, '--window', '1600x900', "$Port"

    if ($Desktop -gt 0) {
        Start-Sleep 3
        try {
            Import-Module -Name VirtualDesktop -DisableNameChecking -EA Stop
            while ((Get-DesktopCount) -lt $Desktop) { New-Desktop | Out-Null }
            $w = Get-Process mirror -EA SilentlyContinue |
                 Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle -match 'view|window|seat' } |
                 Select-Object -Last 1
            if ($w) {
                Move-Window -Desktop (Get-Desktop ($Desktop - 1)) -Hwnd $w.MainWindowHandle | Out-Null
                # Move-Window can drag the current desktop along with the window,
                # which leaves you sitting on VD2 with the control window
                # forwarding your input into the seat. Go back to VD1.
                Start-Sleep 1
                Switch-Desktop -Desktop (Get-Desktop 0)
                Say "control window parked on virtual desktop $Desktop" Green
                Say "  Win+Ctrl+Right to go there and drive the seat" DarkGray
            } else {
                Say "  could not identify the control window -- move it by hand" Yellow
            }
        } catch {
            Say "  VirtualDesktop module unavailable -- move the window by hand" Yellow
        }
    }
}

# -------------------------------------------------------------- verify -----
Start-Sleep 2
Say ""
Say "=== running ===" Cyan
Get-Process mirror, sdl-freerdp -EA SilentlyContinue |
    Select-Object Name, Id, @{n='MB';e={[math]::Round($_.WorkingSet64/1MB)}} |
    Format-Table -AutoSize

$a = & "$Root\hydra-shm.ps1" 6>&1 | Out-String
Start-Sleep 4
$b = & "$Root\hydra-shm.ps1" 6>&1 | Out-String
$f1 = if ($a -match 'frame=(\d+)')  { [int]$Matches[1] } else { -1 }
$f2 = if ($b -match 'frame=(\d+)')  { [int]$Matches[1] } else { -1 }
$c1 = if ($a -match 'curSeq=(\d+)') { [int]$Matches[1] } else { -1 }
$c2 = if ($b -match 'curSeq=(\d+)') { [int]$Matches[1] } else { -1 }
Say ("frames {0}/s   cursor {1}/s   ready={2}" -f `
     [math]::Round(($f2-$f1)/4,1), [math]::Round(($c2-$c1)/4,1), `
     $(if ($b -match 'ready=(\d+)') { $Matches[1] } else { '?' })) DarkGray
if ($b -match 'STALLED') { Say "  STALLED is set -- capture is attached but EnumOutputs is empty" Red }
Say "  (0 frames/s just means seat $Seat's screen is static -- not a fault)" DarkGray

Say ""
Say "The student's wireless keyboard and mouse drive seat $Seat directly." Cyan
Say "No window, no focus, no virtual desktop involved." DarkGray
Say ""
Say "Stop everything:  .\hydra6.ps1 -Stop" Cyan
Say "Panic:            type  Stop-Service Hydra  and press Enter (works blind)" DarkGray
