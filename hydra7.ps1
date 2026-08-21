# hydra7.ps1 -- mode 7 cold start, one command.
#
# MODE 7 is mode 6 with the machinery removed.
#
#   sdl-freerdp  --fullscreen on the student's physical panel-->  done.
#
# No virtual display. No session_capture. No shared ring. No mirror. The client
# IS the seat's screen, and it plays the seat's audio itself through its winmm
# rdpsnd backend.
#
# WHY THIS IS ALLOWED NOW. Modes 2-6 are all increasingly elaborate answers to
# one problem: mstsc suppresses output when covered or minimised, so the panel
# froze. `-suppress-output` (added 2026-08-16) solves that directly. A fullscreen
# client on a monitor with nothing else on it is never covered anyway.
#
# The project already knew this. hydra-no-overlay-needed\seats.toml calls
# display_mode="off" plus a fullscreen client "the configuration that actually
# works" -- it was abandoned only because mstsc froze.
#
# UNCHANGED: input isolation and audio are independent of the display path.
# seat_router -> agent:B injects the wireless pair into the session regardless.
#
# USAGE (elevated, from the Hydra Shell)
#   .\hydra7.ps1
#   .\hydra7.ps1 -Monitor 2      # skip auto-detect, use this FreeRDP index
#   .\hydra7.ps1 -Stop

param(
    [int]$Monitor = 0,          # 0 = auto-detect the seat panel
    [switch]$Stop,
    [string]$Seat = 'B',
    [string]$User = 'teacher',
    [switch]$NoAudioPin,
    [string]$Root = 'C:\Programs\hydra'
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
        # A merely DISCONNECTED session is held by RDP-Wrapper and only a reboot
        # clears it. Log it off properly.
        if ($sid) { Say "  logging off $User session $sid" Yellow; logoff $sid 2>$null }
    }
    Say "stopped." Green
}

if ($Stop) { Stop-Everything; return }
Stop-Everything
Say ""

# ---------------------------------------------------------- preconditions --
if ((& sc.exe qc TermService | Out-String) -notmatch 'WIN32_OWN_PROCESS') {
    Say "TermService is not type= own. RDP-Wrapper's ServiceDll will not load," Red
    Say "you get ONE session, and it shows up as ERRCONNECT_ACTIVATION_TIMEOUT." Red
    Say "  sc.exe config TermService type= own      (then reboot)" Red
    return
}

$dm = (Select-String -Path "$Root\dist\seats.toml" -Pattern '^display_mode' | Select-Object -First 1).Line
if ($dm -notmatch '"off"') {
    Say "display_mode is not `"off`" -- currently: $dm" Yellow
    Say "Mode 7 needs it off, or hydrad launches a capture agent that nothing reads." Yellow
    Say "  (Get-Content seats.toml -Raw) -replace '(?m)^display_mode = `".*`"', 'display_mode = `"off`"' | Set-Content seats.toml -NoNewline" Yellow
    Say "  .\setup.ps1" Yellow
    return
}

# ------------------------------------------------------------- service -----
# Still needed: it runs seat_router and agent:B, which are what actually give
# the student their keyboard and mouse.
Say "starting Hydra service ..." Cyan
Start-Service Hydra
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep 1
    if ((& "$Root\dist\hydractl.exe" status 2>&1 | Out-String) -notmatch 'not reachable') { break }
}

# ----------------------------------------------------------- audio pin -----
# MUST run BEFORE the client starts -- a per-app output change will not take on
# an audio stream that is already open (audio-pin.ps1 line 23).
#
# In mode 7 the CLIENT plays the seat's audio, through FreeRDP's winmm rdpsnd
# backend, into the console session. audio_bridge is not involved and abren will
# sit at "waiting for the shared ring" -- that is expected, not a fault.
if (-not $NoAudioPin -and (Test-Path "$Root\audio-pin.ps1")) {
    Say "applying audio pin ..." Cyan
    try { & "$Root\audio-pin.ps1" -Apply -App 'sdl-freerdp.exe' | Out-Null }
    catch { Say "  audio-pin failed: $_" Yellow }
}

# -------------------------------------------------------- monitor index ----
# FreeRDP's own 1-based enumeration, NOT the Windows DISPLAY number, and it
# shifts between boots. Auto-detect: the seat panel is the entry that is neither
# primary (marked *) nor the virtual display.
$mon = & "$Root\dist\freerdp\sdl-freerdp.exe" /list:monitor 2>&1 | Out-String
$lines = $mon -split "`r?`n"
if ($Monitor -eq 0) {
    foreach ($line in $lines) {
        if ($line -match '^\s*\[(\d+)\]\s+\[([^\]]+)\]' -and $line -notmatch '^\s*\*' -and $Matches[2] -notmatch 'VDD') {
            $Monitor = [int]$Matches[1]
            Say "seat panel: FreeRDP monitor $Monitor  [$($Matches[2])]" Green
            break
        }
    }
}
if ($Monitor -eq 0) {
    Say "could not identify the seat panel. Pick one and pass -Monitor <n>:" Red
    Write-Host $mon
    return
}

# ------------------------------------------------------------- client ------
# /sound            REQUIRED. Without it there is no audio channel at all.
# -suppress-output  the flag mode 7 rests on -- the client keeps pulling frames
#                   regardless of what it thinks is visible.
# /scale:140        session DPI, so the seat's UI is readable at 1:1.
# /d:               login focus on the password field, not the domain field.
# /f /monitors:N    fullscreen on the seat's panel. This IS the seat's display.
Say ""
Say "starting the client fullscreen on the seat panel -- LOG IN AS $User" Cyan
Say "  (echo is off; a typo shows as ERRCONNECT_LOGON_FAILURE)" DarkGray
Say "  its log stays open in the new window -- that is where connection errors" DarkGray
Say "  and codec warnings appear. Leave it open." DarkGray

# Launched inside a -NoExit PowerShell window rather than detached, so its
# output is READABLE. Start-Process on the exe directly swallows everything --
# no ERRCONNECT reason, no rdpsnd backend line, no codec warnings. Same approach
# hydra-view.ps1 uses for hydrardp.
$clientArgs = "/v:127.0.0.2 /u:$User /d: /cert:ignore /sound -suppress-output /scale:140 +auto-reconnect /gfx:rfxc /network:lan /f /monitors:$Monitor"
$cmd = "`$host.UI.RawUI.WindowTitle = 'Hydra seat $Seat -- client log'; " +
       "& '$Root\dist\freerdp\sdl-freerdp.exe' $clientArgs"
Start-Process powershell -ArgumentList '-NoExit', '-Command', $cmd

Say "waiting for the seat session ..." Yellow
$ok = $false
for ($i = 0; $i -lt 90; $i++) {
    Start-Sleep 2
    if ((& query session | Out-String) -match "$User\s+\d+\s+Active") { $ok = $true; break }
}
if (-not $ok) { Say "no $User session after 3 minutes." Red; return }
Say "seat session up." Green

# -------------------------------------------------------------- pin --------
# A fullscreen client belongs to the virtual desktop it was launched from, and
# virtual desktops span all monitors -- so switching away hides the seat's
# screen from the student. Pinning puts it on every desktop.
Start-Sleep 3
try {
    Import-Module -Name VirtualDesktop -DisableNameChecking -EA Stop
    $h = (Get-Process sdl-freerdp -EA SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1).MainWindowHandle
    if ($h) { Pin-Window -Hwnd $h | Out-Null; Say "client pinned to all virtual desktops" Green }
    else    { Say "  no client window found to pin" Yellow }
} catch {
    Say "  could not pin the client -- if the panel goes blank when you switch" Yellow
    Say "  virtual desktops, that is why." Yellow
}

# ------------------------------------------------------------- verify ------
Start-Sleep 2
Say ""
$st = & "$Root\dist\hydractl.exe" status 2>&1 | Out-String
Write-Host $st -ForegroundColor DarkGray
if ($st -match 'capture:B') {
    Say "capture:B is running -- display_mode is not off. Harmless but wasteful." Yellow
}
Say "expected in mode 7: router, agent:B, abcap:B, abren:B -- and NO capture:B" DarkGray

Say ""
Say "The student's wireless keyboard and mouse drive seat $Seat directly." Cyan
Say "No window, no focus, no virtual desktop involved." DarkGray

# Mouse button swap is PER USER, and seat B runs as a different account -- so a
# left-handed console does not make seat B left-handed. Crossing onto the seat's
# panel silently flips the buttons back, which is disorienting mid-lesson.
$mineSwapped = (Get-ItemProperty 'HKCU:\Control Panel\Mouse' SwapMouseButtons -EA SilentlyContinue).SwapMouseButtons
if ($mineSwapped -eq '1') {
    Say ""
    Say "NOTE: your console mouse is left-handed. Seat $Seat is a separate user," Yellow
    Say "so it has its own setting. If the buttons flip when you cross onto the" Yellow
    Say "seat's panel, set it once INSIDE the seat:" Yellow
    Say "  Settings > Bluetooth & devices > Mouse > Primary mouse button: Right" DarkGray
    Say "It persists in $User's profile after that." DarkGray
}
Say ""
Say "To reach seat $Seat yourself, move your cursor onto its panel." DarkGray
Say ""
Say "Stop:   .\hydra7.ps1 -Stop" Cyan
Say "Panic:  type  Stop-Service Hydra  and press Enter (works blind)" DarkGray

# --------------------------------------------------------- audio note ------
$pin = Get-Content "$Root\audio-pin.json" -Raw -EA SilentlyContinue
if ($pin -and $pin -notmatch 'intcdaud') {
    Say ""
    Say "AUDIO PIN IS NOT SET TO THE MONITOR." Yellow
    Say "audio-pin.json names no intcdaud entry, so seat $Seat's sound follows the" Yellow
    Say "console's output device. Fix it ONCE:" Yellow
    Say "  1. play something in seat $Seat" DarkGray
    Say "  2. Settings > Sound > Volume mixer > sdl-freerdp.exe" DarkGray
    Say "     > Output device > 2770 (Intel Display Audio)" DarkGray
    Say "  3. Remove-Item $Root\audio-pin.json -Force" DarkGray
    Say "  4. .\audio-pin.ps1 -Save -App 'sdl-freerdp.exe'" DarkGray
    Say "One entry naming intcdaud means it is right. -Apply then restores it" DarkGray
    Say "on every launch, before the client opens its stream." DarkGray
}
