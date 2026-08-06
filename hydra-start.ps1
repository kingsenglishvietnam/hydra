# hydra-start.ps1 -- bring the whole seat up, in the right order, in one command.
#
# WHY THIS EXISTS
#   mirror only ever works reliably when it is started AFTER the capture agent is
#   already publishing frames. Started earlier -- as a service child, as a logon
#   scheduled task, or from the Startup folder -- it ends up stuck: ~1 MB working
#   set with CPU climbing, nothing on the panel. Started by hand once everything
#   else is up, it works every single time, immediately.
#
#   Rather than keep guessing at launch contexts, this script just enforces the
#   order that is known to work: service first, wait for capture to actually be
#   running, then launch mirror. Deterministic, and one command.
#
# USAGE (elevated PowerShell):
#   .\hydra-start.ps1
#
# PREREQUISITES, in this order:
#   1. You are logged in as seat 1.
#   2. Teacher's RDP session is CONNECTED and LOGGED IN (not sitting at a lock
#      screen -- seat B's input cannot reach a secure desktop).
#   3. The mstsc window is open and on-screen (minimizing or moving it
#      off-screen freezes the panel).

param(
    [int]$Desktop    = 2,          # Windows virtual desktop to park this console on (0 = don't move)
    [string]$Seat    = 'B',
    [string]$Monitor = '\\.\DISPLAY2',
    [string]$RdpFile = "$PSScriptRoot\teacher.rdp",

    # Which RDP client holds the seat's session open.
    #
    #   "mstsc"   Microsoft's client. Works, but SUPPRESSES OUTPUT when it is not
    #             visible -- minimized, covered, or on an inactive virtual
    #             desktop. The seat's desktop then stops being composed, Desktop
    #             Duplication sees nothing, and the panel freezes on its last
    #             frame while every process still looks healthy. That is a client
    #             decision we cannot influence.
    #   "freerdp" SDL-FreeRDP. Open source, so if it does not suppress output the
    #             freeze disappears as a category rather than as a bug.
    # FREERDP IS THE DEFAULT, and this is the fix for the panel freeze rather
    # than a preference.
    #
    # Measured with a probe that generates its own activity (mirror --probe,
    # which nudges the seat's cursor so an IDLE desktop is not mistaken for a
    # frozen one): mstsc stops the seat's desktop being composed as soon as its
    # window is minimized or covered -- it sends a Suppress Output PDU -- while
    # SDL-FreeRDP keeps streaming in both cases. Everything built to work around
    # that (topmost thumbnails, the client watchdog, the window-size hunt) exists
    # only for mstsc.
    #
    # "mstsc" is kept as a fallback for machines without FreeRDP installed.
    [ValidateSet('mstsc','freerdp')]
    [string]$Client = 'freerdp',
    # Prefer the copy vendored into the tree (see vendor-freerdp.ps1); fall back
    # to an MSYS2 install if that has not been done.
    [string]$FreeRdpPath = '',
    [string]$FreeRdpUser = 'teacher',
    [string]$FreeRdpSize = '1920x1080',
    # Extra FreeRDP options, passed straight through. Useful ones:
    #   +smart-sizing        scale the session to the window (resizable, stretched)
    #   /smart-sizing:WxH    scale to a specific size
    #   +dynamic-resolution  resize the SESSION to match the window instead
    #                        (crisper, but capture cost follows the window size)
    #   /f                   fullscreen on one monitor
    #   /monitors:N          pick which monitor -- known to be awkward
    #   /gdi:hw              hardware rendering
    # e.g.  .\hydra-start.ps1 -Client freerdp -FreeRdpArgs '+smart-sizing','/gdi:hw'
    [string[]]$FreeRdpArgs = @(),

    # How the client window is placed:
    #   "thumbnail" (default) 320x200, TOPMOST, top-right corner. It cannot be
    #               covered, so the panel cannot freeze, and mirror's view window
    #               can be maximized underneath it.
    #   "maximized" fill the screen in a normal frame you can Alt-Tab out of.
    #               Pair with -FreeRdpArgs '+smart-sizing' so the session scales
    #               to the window. WARNING: whatever you Alt-Tab to will COVER
    #               it, and a covered client stops requesting updates -- the
    #               seat's desktop stops being composed and the panel freezes.
    #   "none"      leave the window exactly where the client puts it.
    [ValidateSet('thumbnail','maximized','none')]
    [string]$ClientWindow = 'thumbnail',
    [string]$TeacherUser = 'teacher',
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# --- 0. move this console to its own virtual desktop ---------------------------
# Keeps the Hydra console, its output and the RDP thumbnail off the desktop you
# actually teach from. Windows exposes no scriptable API for virtual desktops, so
# this uses the VirtualDesktop module if present and silently skips if not --
# never worth failing the whole startup over a cosmetic move.
#   Install once with:  Install-Module VirtualDesktop -Scope CurrentUser -Force
if ($Desktop -gt 0) {
    try {
        if (-not (Get-Module -ListAvailable -Name VirtualDesktop)) { throw "not installed" }
        Import-Module VirtualDesktop -ErrorAction Stop

        while ((Get-DesktopCount) -lt $Desktop) { New-Desktop | Out-Null }

        $hwnd = (Get-Process -Id $PID).MainWindowHandle
        if ($hwnd -ne 0) {
            Move-Window -Desktop (Get-Desktop ($Desktop - 1)) -Hwnd $hwnd | Out-Null
            Switch-Desktop ($Desktop - 1)
            Write-Host "console moved to virtual desktop $Desktop" -ForegroundColor DarkGray
        }
    } catch {
        Write-Host "virtual desktop $Desktop unavailable (Install-Module VirtualDesktop) -- staying put" -ForegroundColor DarkGray
    }
}
$exe  = Join-Path $root 'dist\mirror.exe'
$ctl  = Join-Path $root 'dist\hydractl.exe'

function Say($m, $c='Gray') { Write-Host $m -ForegroundColor $c }

# --- 1. clear any stale mirror -------------------------------------------------
# A stuck mirror holds the exe and fights the new one for the panel.
$old = Get-Process mirror -ErrorAction SilentlyContinue
if ($old) {
    Say "stopping $($old.Count) existing mirror process(es)" 'Yellow'
    $old | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

# --- 1a. restore the audio assignment ------------------------------------------
# Must happen BEFORE mstsc launches: a per-app output change does not take effect
# on an audio stream that is already open. Windows resets this on every reboot,
# so it gets re-applied every start. Capture the good state once with:
#     .\audio-pin.ps1 -Save
$audioPin = Join-Path $root 'audio-pin.ps1'
if (Test-Path $audioPin) {
    try { & $audioPin -Apply } catch { Say "  audio-pin failed: $_" 'Yellow' }
}

# --- 1b. teacher's RDP session -------------------------------------------------
# capture duplicates TEACHER'S desktop, so that session must exist and be logged
# in before anything else is worth starting. Launch the client if it isn't
# already up. NOTE: this can only log in unattended if the .rdp has saved
# credentials -- otherwise mstsc shows its prompt and waits for you.
$clientProc = if ($Client -eq 'freerdp') { 'sdl-freerdp' } else { 'mstsc' }
$mstsc = Get-Process $clientProc -ErrorAction SilentlyContinue
if (-not $mstsc) {
    if ($Client -eq 'freerdp') {
        if (-not $FreeRdpPath) {
            foreach ($cand in @("$PSScriptRoot\dist\freerdp\sdl-freerdp.exe",
                                'C:\msys64\mingw64\bin\sdl-freerdp.exe')) {
                if (Test-Path $cand) { $FreeRdpPath = $cand; break }
            }
        }
        if (-not $FreeRdpPath -or -not (Test-Path $FreeRdpPath)) {
            Say "FreeRDP not found at $FreeRdpPath" 'Red'
            Say "  winget install MSYS2.MSYS2" 'Yellow'
            Say "  then in MSYS2 MINGW64:  pacman -S mingw-w64-x86_64-freerdp" 'Yellow'
            return
        }
        Say "launching FreeRDP: $FreeRdpPath"
        # No /p: on the command line -- it would be readable by every process on
        # the machine. FreeRDP prompts for the password instead.
        # /sound IS REQUIRED, even though audio_bridge carries the audio.
        #
        # Dropping it seemed right -- the bridge already delivers the seat's
        # sound, and letting the client play it too gives an echo. But removing
        # /sound removes the seat session's audio ENDPOINT entirely, and the
        # bridge works by loopback-recording that endpoint. No endpoint, nothing
        # to record, silence everywhere. "No audio device is installed" in the
        # seat's session is the symptom.
        #
        # So: ask for audio (the session gets an endpoint), and stop the CLIENT
        # from playing it. /d: gives an empty domain so the credential prompt
        # starts on the password field.
        $fa = @("/v:127.0.0.2", "/u:$FreeRdpUser", "/d:", "/size:$FreeRdpSize",
                "/cert:ignore", "/sound", "+auto-reconnect") + $FreeRdpArgs
        Say "  args: $($fa -join ' ')" 'DarkGray'
        Start-Process $FreeRdpPath -ArgumentList $fa
    } elseif (Test-Path $RdpFile) {
        Say "launching RDP client: $RdpFile"
        Start-Process mstsc.exe -ArgumentList "`"$RdpFile`""

        # Bring it to the FOREGROUND once its window exists.
        #
        # Start-Process doesn't guarantee focus, and this script usually runs from
        # an elevated console that keeps it -- so the client comes up behind
        # everything, which matters when it's showing a credential prompt you're
        # supposed to type into.
        #
        # SetForegroundWindow alone is refused unless the calling thread already
        # owns the foreground, so we attach to the foreground thread's input queue
        # first, which is the standard way round that restriction.
        Add-Type -Name Fg -Namespace HydraFg -MemberDefinition @"
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
"@ -ErrorAction SilentlyContinue

        $deadlineFg = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadlineFg) {
            $mw = Get-Process $clientProc -ErrorAction SilentlyContinue |
                  Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
            if ($mw) {
                try {
                    $h  = $mw.MainWindowHandle
                    $fg = [HydraFg.Fg]::GetForegroundWindow()
                    $t1 = [HydraFg.Fg]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
                    $t2 = [HydraFg.Fg]::GetCurrentThreadId()
                    [void][HydraFg.Fg]::AttachThreadInput($t2, $t1, $true)
                    [void][HydraFg.Fg]::ShowWindow($h, 9)      # SW_RESTORE
                    [void][HydraFg.Fg]::BringWindowToTop($h)
                    [void][HydraFg.Fg]::SetForegroundWindow($h)
                    [void][HydraFg.Fg]::AttachThreadInput($t2, $t1, $false)
                    Say "RDP client brought to the foreground" 'Green'
                } catch { Say "  couldn't focus the RDP window: $_" 'Yellow' }
                break
            }
            Start-Sleep -Milliseconds 300
        }
    } else {
        Say "no .rdp at $RdpFile -- connect teacher's session manually" 'Yellow'
    }
} else {
    Say "RDP client already running"
}

# Wait for the session to actually EXIST. A connected-but-locked session still
# shows here, which is why the capture wait below is the real gate.
Say "waiting for $TeacherUser's session ..."
$sdeadline = (Get-Date).AddSeconds($TimeoutSec)
$sessOk = $false
while ((Get-Date) -lt $sdeadline) {
    $q = (query session 2>&1 | Out-String)
    if ($q -match [regex]::Escape($TeacherUser)) { $sessOk = $true; break }
    Start-Sleep -Seconds 1
}
if ($sessOk) { Say "$TeacherUser session present" 'Green' }
else {
    Say "no session for '$TeacherUser' after $TimeoutSec s." 'Red'
    Say "If mstsc is showing a password prompt, log teacher in and re-run." 'Red'
    return
}

# --- 1c. wait for teacher to actually be LOGGED IN -----------------------------
# "Session present" is not the same as "logged in": a session sitting at the lock
# screen shows up in `query session` exactly like a live one. The reliable tell is
# LogonUI.exe -- it runs in a session precisely WHILE that session is showing the
# lock/credential screen, and exits once the desktop is up.
#
# This matters because a locked session is a SECURE DESKTOP: seat B's SendInput
# is refused with ERROR_ACCESS_DENIED, and capture has no ordinary desktop to
# duplicate. Everything downstream fails in confusing ways if we press on.
$tsid = $null
foreach ($line in (query session 2>&1)) {
    if ($line -match "\s$([regex]::Escape($TeacherUser))\s+(\d+)\s") { $tsid = [int]$Matches[1]; break }
}

if ($null -ne $tsid) {
    Say "waiting for $TeacherUser to finish logging in (session $tsid) ..."
    $ldeadline = (Get-Date).AddSeconds($TimeoutSec)
    $loggedIn = $false
    while ((Get-Date) -lt $ldeadline) {
        $lui = Get-Process LogonUI -ErrorAction SilentlyContinue |
               Where-Object { $_.SessionId -eq $tsid }
        if (-not $lui) { $loggedIn = $true; break }
        Start-Sleep -Seconds 1
    }
    if ($loggedIn) { Say "$TeacherUser logged in" 'Green' }
    else {
        Say "$TeacherUser is still at the lock screen after $TimeoutSec s." 'Red'
        Say "Click into the mstsc window, log teacher in, then re-run this script." 'Red'
        Say "(Tip: save teacher's credentials in the .rdp to make this unattended.)" 'Yellow'
        return
    }
} else {
    Say "couldn't determine $TeacherUser's session id; continuing" 'Yellow'
}

# --- 2. service ----------------------------------------------------------------
$svc = Get-Service Hydra -ErrorAction SilentlyContinue
if (-not $svc) { throw "Hydra service not installed -- run .\setup.ps1 first" }
if ($svc.Status -ne 'Running') {
    Say "starting Hydra service..."
    Start-Service Hydra
} else {
    Say "Hydra service already running"
}

# --- 3. wait for capture to actually be up -------------------------------------
# This is the whole point: mirror must not start before frames are being
# published. capture:B only reaches "running" once teacher's session exists.
Say "waiting for capture:$Seat ..."
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$ready = $false
while ((Get-Date) -lt $deadline) {
    $status = & $ctl status 2>&1 | Out-String
    if ($status -match "capture:$Seat\s*:\s*running") { $ready = $true; break }
    Start-Sleep -Seconds 1
}

if (-not $ready) {
    Say "capture:$Seat did not reach 'running' within $TimeoutSec s." 'Red'
    Say "Almost always means teacher's RDP session isn't connected/logged in yet." 'Red'
    Say "Connect it, log teacher in, then re-run this script." 'Red'
    & $ctl status
    return
}
Say "capture:$Seat running" 'Green'

# Give the producer a moment to publish its first frames before mirror looks.
Start-Sleep -Seconds 2

# --- 4. mirror -----------------------------------------------------------------
# Healthy mirror allocates D3D resources (~70-98 MB). A stuck one sits at a few
# MB and burns CPU. It reliably works when started after the producer is really
# publishing, so rather than declare failure, kill and retry a couple of times --
# that has always been enough.
$mb = 0
for ($try = 1; $try -le 3; $try++) {
    Say "starting mirror (attempt $try)..."
    Start-Process $exe -ArgumentList $Seat, $Monitor -WindowStyle Minimized
    Start-Sleep -Seconds 4

    $m = Get-Process mirror -ErrorAction SilentlyContinue
    if (-not $m) { Say "mirror did not start." 'Red'; continue }

    $mb = [math]::Round(($m | Measure-Object WorkingSet64 -Maximum).Maximum / 1MB, 1)
    if ($mb -gt 40) { break }

    Say "  came up stuck (${mb} MB); retrying" 'Yellow'
    $m | Stop-Process -Force
    Start-Sleep -Seconds 2
}

if ($mb -gt 40) {
    Say "mirror healthy (${mb} MB) -- panel should be live" 'Green'
} else {
    Say "mirror still stuck after 3 attempts (${mb} MB, expected >40 MB)." 'Red'
    Say "Run it in the foreground to see why:" 'Red'
    Say "  .\dist\mirror.exe $Seat `"$Monitor`"" 'Red'
}

# --- 5. tuck the RDP client away, and PIN it -----------------------------------
# Small and VISIBLE, never minimized. Anything that stops the RDP client being
# composited makes it stop requesting screen updates, so capture duplicates a
# stale desktop and the panel freezes. Three routes to that same failure, all
# measured: minimizing it, moving it off-screen, and leaving it on an INACTIVE
# virtual desktop. The last one is why moving this console to desktop 2 froze the
# panel -- so the client gets pinned to every desktop.
$minify = Join-Path $root 'minify-mstsc.ps1'
if (Test-Path $minify) {
    Say "tucking the RDP window into a corner..."
    # -Margin 0 = the entire work area, which looks the same as maximized.
    # mstsc IGNORES programmatic maximize (both ShowWindow(SW_MAXIMIZE) and
    # WM_SYSCOMMAND/SC_MAXIMIZE return success and do nothing), and its saved
    # winposstr is overridden by this call anyway, so sizing it explicitly is the
    # only thing that actually decides where the window ends up.
    # SMALL AND TOPMOST, whichever client it is.
    #
    # The client only has to hold the session open -- you interact through
    # mirror's view window. But it must never be COVERED: a covered client stops
    # requesting updates, the seat's desktop stops being composed, and the panel
    # freezes. Windows still reports such a window as visible, so nothing detects
    # it. Measured with both mstsc and SDL-FreeRDP.
    #
    # Topmost thumbnail solves it: it cannot be covered, so the view window can
    # be maximized underneath without freezing anything.
    # A FULLSCREEN client needs no placement -- it cannot be covered, so the
    # freeze cannot occur, and moving it would only fight the client. Detect it
    # and leave well alone.
    $fullscreen = @($FreeRdpArgs) -contains '/f' -or @($FreeRdpArgs) -contains '+f'
    if ($fullscreen -or $ClientWindow -eq 'none') {
        Say "  (client placement skipped)" 'DarkGray'
    } elseif ($ClientWindow -eq 'maximized') {
        try { & $minify -Process $clientProc -Fill -Margin 0 | Out-Null }
        catch { Say "  window placement failed: $_" 'Yellow' }
        Say "  client is maximized but COVERABLE -- if you Alt-Tab over it the" 'Yellow'
        Say "  panel will freeze. Use -ClientWindow thumbnail to avoid that." 'Yellow'
    } else {
        try {
            & $minify -Process $clientProc -TopMost -Width 320 -Height 200 -Corner TopRight | Out-Null
        } catch { Say "  window placement failed: $_" 'Yellow' }
    }
} else {
    Say "minify-mstsc.ps1 not found; leaving the RDP window as-is" 'Yellow'
}

if ($Desktop -gt 0) {
    try {
        Import-Module VirtualDesktop -ErrorAction Stop
        $mp = Get-Process $clientProc -ErrorAction SilentlyContinue |
              Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($mp) {
            Pin-Window -Hwnd $mp.MainWindowHandle | Out-Null
            Say "RDP window pinned to all virtual desktops (keeps the panel live)" 'Green'
        } else {
            Say "no RDP window found to pin" 'Yellow'
        }
    } catch {
        Say "couldn't pin the RDP window -- keep it on the desktop you are using," 'Yellow'
        Say "or the panel will freeze when you switch away." 'Yellow'
    }
}

# --- 6. prime the audio endpoint -----------------------------------------------
# The seat's Remote Audio endpoint goes bad when idle: the FIRST application to
# open it gets silence, a second app works, and then the first one does too.
# Playing ONE short sound in the seat's session makes that sound the first
# opener, so whatever the user launches next is second -- and works.
#
# Uses hydrad's audiofix launch path, which runs a process in the seat's session
# with the console admin's elevated token. (The restart itself never worked from
# any context; the LAUNCH mechanism is sound and is what's reused here.)
#
# If the endpoint turns out to go idle again mid-session, switch to the
# permanent version instead -- set  audio_prime = "keepalive"  in seats.toml,
# which holds a silent stream open all session so it can never go idle.
Say "priming the audio endpoint ..."
try {
    & $ctl chime $Seat
    Start-Sleep -Seconds 3
    Say "audio primed" 'Green'
} catch {
    Say "  couldn't prime audio: $_" 'Yellow'
    Say "  play any sound in the seat's session before opening a browser." 'Yellow'
}

# NOTE: there is no audio-endpoint restart here any more.
#
# Restarting Audiosrv was tried from four different contexts -- the console
# session, a SYSTEM scheduled task in session 0, a SYSTEM token inside the seat's
# session, and the console admin's ELEVATED token inside the seat's session (the
# exact combination that works when typed by hand). None of them fixed the
# first-app-gets-silence problem. A restart leaves the endpoint IDLE, which just
# hands the problem to whoever opens it next; the manual successes had another
# app already holding the endpoint open.
#
# `keepalive:<seat>` now holds a silent stream open permanently instead, so the
# endpoint never goes idle and no application is ever the first opener.
# `hydractl audiofix <seat>` still exists for manual use, but should not be
# needed.

Write-Host ""
& $ctl status
Write-Host ""
Say "Checklist:" 'Cyan'
Say "  panel shows teacher's desktop, cursor visible over the Start menu" 'Cyan'
Say "  teacher audio -> monitor, your audio -> laptop, simultaneously" 'Cyan'
Say "  RDP window small and VISIBLE -- do NOT minimize it, that freezes the panel" 'Cyan'
Say "  resize it with:  .\minify-mstsc.ps1 -Width 800 -Height 500" 'Cyan'
