# minify-mstsc.ps1 -- shrink the RDP client to a small corner window.
#
# WHY NOT JUST HIDE IT
#   Desktop Duplication can only capture teacher's session while an RDP client is
#   connected AND actively receiving updates. Two things stop that:
#     - MINIMIZING  -- the client stops requesting screen updates; panel freezes.
#     - MOVING IT OFF-SCREEN -- measured: same result, panel freezes.
#   So the window must stay genuinely on-screen. It does NOT have to be big.
#
#   This shrinks it to a thumbnail in a corner: the session keeps compositing,
#   capture keeps getting frames, and you get your screen back. It also stays
#   useful -- clicking into it lets you drive teacher's session with seat 1's
#   keyboard and mouse, alongside seat B's own devices.
#
# USAGE:
#   .\minify-mstsc.ps1                    # 320x200, bottom-right
#   .\minify-mstsc.ps1 -Width 480 -Height 300
#   .\minify-mstsc.ps1 -Corner TopLeft
#   .\minify-mstsc.ps1 -Restore           # back to 1280x800, centred-ish

param(
    [int]$Width  = 320,
    [int]$Height = 200,
    [ValidateSet('BottomRight','BottomLeft','TopRight','TopLeft')]
    [string]$Corner = 'BottomRight',
    [switch]$Fill,        # fill the LAPTOP screen (not the seat panel)
    [switch]$TopMost,     # keep it above everything -- see the note below
    [switch]$Ghost,       # full-size but INVISIBLE and click-through (see below)
    [int]$GhostAlpha = 1, # 0 is fully transparent; 1 keeps it "shown" to DWM
    [string]$Process = 'mstsc',   # 'mstsc' or 'sdl-freerdp' 
    [switch]$Maximize,    # ask the window manager to maximize (mstsc often ignores this)
    [int]$Margin = 60,    # px kept clear of the screen edge in -Fill mode; 0 = flush
    [switch]$Restore
)

# UNIQUE TYPE NAME PER RUN.
#
# A .NET type cannot be redefined once loaded into a process. Re-running this
# script in the SAME PowerShell session after editing it means the OLD definition
# silently wins: Add-Type errors with TYPE_ALREADY_EXISTS, execution continues,
# and any method added since is "not found" -- while the rest of the script
# appears to work and prints reassuring output. That cost real time twice.
#
# Versioning the name by hand only helps between edits, not between runs. A name
# derived from a fresh GUID is unique every invocation, so the definition loaded
# is always the one in this file.
$tn = "HydraWin_" + [guid]::NewGuid().ToString("N")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class $tn {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr after, int X, int Y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int i, int v);
    [DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
}
"@ -PassThru | Out-Null
$api = [type]$tn   # NOT $w -- that is a width variable further down
Add-Type -AssemblyName System.Windows.Forms

# BECOME DPI-AWARE BEFORE MEASURING ANYTHING.
#
# PowerShell is DPI-unaware by default, so on this machine -- a 3240x2160 panel
# at 350% scaling -- Screen.WorkingArea reports 926x577 LOGICAL pixels. But
# SetWindowPos takes PHYSICAL pixels. Sizing the window to 926x577 physical on a
# 3240-wide screen produces a window about a quarter of the screen, which is
# exactly the "it won't maximize" symptom: the numbers looked right and the
# window was tiny.
#
# The same mismatch explains why  mirror --list  reports the monitor as 1920x1080
# while mirror itself sees 6720x3780 -- one is DPI-aware and the other isn't.
[void]$api::SetProcessDPIAware()

$SWP_NOZORDER   = 0x0004
$SWP_NOACTIVATE = 0x0010
$SWP_NOMOVE     = 0x0002
$SWP_NOSIZE     = 0x0001
$HWND_TOPMOST   = [IntPtr]-1
$SW_RESTORE     = 9

$procs = Get-Process $Process -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 }
if (-not $procs) { Write-Warning "No $Process window found -- is the seat's session connected?"; exit 1 }

# Park it on the screen the SEAT PANEL IS NOT ON.
#
# Assuming "primary" was wrong: if Windows has the external monitor as primary,
# the thumbnail lands directly on the panel and sits on top of mirror's output --
# which looks exactly like mirror having failed. Pick by size instead: the seat
# panel is the larger/other display, so prefer the smallest non-panel screen,
# falling back to primary when there is only one.
$panelW = 1920   # captured desktop width; the panel is whatever ISN'T the laptop
$all = [System.Windows.Forms.Screen]::AllScreens
if ($all.Count -gt 1) {
    # the laptop screen is the one marked Primary UNLESS primary is the big panel
    $cand = $all | Sort-Object { $_.Bounds.Width * $_.Bounds.Height }
    $scr  = $cand[0].WorkingArea        # smallest screen = the laptop
} else {
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
}
Write-Host ("parking on screen {0}x{1} at {2},{3}" -f $scr.Width,$scr.Height,$scr.X,$scr.Y) -ForegroundColor DarkGray

# CLAMP every requested size to the screen, with a margin.
#
# Sizing the client to exactly (or beyond) the work area tips mstsc into
# FULLSCREEN, where it grabs input and there is no obvious way back -- and if it
# lands on the seat panel it also covers mirror. A margin keeps it a normal
# window no matter what numbers get passed in.
# -Margin 0 sizes the window to the entire work area, which looks the same as
# maximized. That matters because mstsc IGNORES programmatic maximize: both
# ShowWindow(SW_MAXIMIZE) and WM_SYSCOMMAND/SC_MAXIMIZE return success and do
# nothing. Sizing it ourselves is the only thing that actually works.
#
# The default margin is not superstition: sizing a window BEYOND the screen tips
# mstsc into RDP fullscreen, which grabs input with no obvious way out. A margin
# of 0 is exactly the work area and is safe; anything larger is clamped below.
$margin = [Math]::Max(0, $Margin)
$maxW = [Math]::Max(320, $scr.Width  - $margin)
$maxH = [Math]::Max(200, $scr.Height - $margin)
if ($Width  -gt $maxW) { Write-Host "clamping width $Width -> $maxW"   -ForegroundColor DarkYellow; $Width  = $maxW }
if ($Height -gt $maxH) { Write-Host "clamping height $Height -> $maxH" -ForegroundColor DarkYellow; $Height = $maxH }

foreach ($p in $procs) {
    $h = $p.MainWindowHandle
    if ($api::IsIconic($h)) {
        [void]$api::ShowWindow($h, $SW_RESTORE)   # minimized = frozen panel
        Start-Sleep -Milliseconds 300
    }

    if ($Maximize) {
        # A real maximize, via the window manager, rather than sizing to the work
        # area. Sizing to exactly the screen can tip mstsc into RDP FULLSCREEN,
        # which grabs input and has no obvious way out. SC_MAXIMIZE does not.
        #
        # Maximizes on whichever monitor the window currently occupies, so move
        # it to the laptop first (.\minify-mstsc.ps1) or it will maximize over
        # the seat panel and cover mirror's output.
        [void]$api::SendMessage($h, 0x112, [IntPtr]0xF030, [IntPtr]0)  # WM_SYSCOMMAND, SC_MAXIMIZE
        Write-Host ("mstsc pid {0}: asked to maximize -- note mstsc often ignores this;" -f $p.Id) -ForegroundColor Yellow
        Write-Host "  use  .\minify-mstsc.ps1 -Fill -Margin 0  instead" -ForegroundColor Yellow
        continue
    }

    if ($Fill) {
        # Fill the laptop's work area. Deliberately NOT a real maximize/fullscreen:
        # maximizing can land the window on the seat panel, where it would sit on
        # top of mirror's output and hide teacher's desktop. Sizing it explicitly
        # to the laptop screen keeps the panel clear, and the window still counts
        # as visibly composited so the RDP client keeps pulling frames.
        # Deliberately a margin short of the full work area -- filling it exactly
        # is what triggers fullscreen.
        $fx = if ($margin -gt 0) { $scr.X + [int]($margin/2) } else { $scr.X }
        $fy = if ($margin -gt 0) { $scr.Y + [int]($margin/2) } else { $scr.Y }
        [void]$api::SetWindowPos($h, [IntPtr]::Zero, $fx, $fy, $maxW, $maxH,
                                       $SWP_NOZORDER -bor $SWP_NOACTIVATE)
        Write-Host ("mstsc pid {0} -> {1}x{2} at {3},{4} (still windowed)" -f `
                    $p.Id,$maxW,$maxH,$fx,$fy) -ForegroundColor Green
        continue
    }

    if ($Restore) {
        # Undo ghost mode if it was applied, or the window stays invisible.
        $GWL_EXSTYLE = -20
        $ex = $api::GetWindowLong($h, $GWL_EXSTYLE)
        [void]$api::SetWindowLong($h, $GWL_EXSTYLE, $ex -band (-bnot (0x00080000 -bor 0x00000020)))
        $w = [Math]::Min(1280, $maxW); $ht = [Math]::Min(800, $maxH)
        $x = $scr.X + [int](($scr.Width  - $w)  / 2)
        $y = $scr.Y + [int](($scr.Height - $ht) / 2)
        [void]$api::SetWindowPos($h, [IntPtr]::Zero, $x, $y, $w, $ht,
                                       $SWP_NOZORDER -bor $SWP_NOACTIVATE)
        Write-Host ("mstsc pid {0} -> restored {1}x{2} at {3},{4}" -f $p.Id,$w,$ht,$x,$y) -ForegroundColor Green
        continue
    }

    switch ($Corner) {
        'BottomRight' { $x = $scr.Right  - $Width - 8; $y = $scr.Bottom - $Height - 8 }
        'BottomLeft'  { $x = $scr.Left   + 8;          $y = $scr.Bottom - $Height - 8 }
        'TopRight'    { $x = $scr.Right  - $Width - 8; $y = $scr.Top    + 8 }
        'TopLeft'     { $x = $scr.Left   + 8;          $y = $scr.Top    + 8 }
    }
    [void]$api::SetWindowPos($h, [IntPtr]::Zero, $x, $y, $Width, $Height,
                                   $SWP_NOZORDER -bor $SWP_NOACTIVATE)
    Write-Host ("mstsc pid {0} -> {1}x{2} at {3},{4} ({5})" -f $p.Id,$Width,$Height,$x,$y,$Corner) -ForegroundColor Yellow
}

# GHOST MODE: full-size, invisible, click-through.
#
# The freeze happens because a COVERED client stops requesting screen updates --
# the seat's desktop then stops being composed and the panel holds its last
# frame. Windows still reports such a window as visible, so nothing detects it.
#
# The thumbnail approach avoids this by being small and topmost, but it costs a
# corner of the screen and something to look at. Ghost mode is better: leave the
# client MAXIMIZED so it is unambiguously unoccluded, then make it invisible with
# WS_EX_LAYERED at near-zero alpha and click-through with WS_EX_TRANSPARENT. The
# client believes it is fully visible and keeps streaming; you see and click the
# view window behind it.
#
# Alpha 1 rather than 0 deliberately: a fully transparent layered window can be
# treated as not-rendering by DWM, which would defeat the whole point. 1/255 is
# invisible to the eye and unambiguous to the compositor.
if ($Ghost) {
    $GWL_EXSTYLE     = -20
    $WS_EX_LAYERED   = 0x00080000
    $WS_EX_TRANSPARENT = 0x00000020
    $LWA_ALPHA       = 0x2
    foreach ($p in $procs) {
        $h = $p.MainWindowHandle
        $ex = $api::GetWindowLong($h, $GWL_EXSTYLE)
        [void]$api::SetWindowLong($h, $GWL_EXSTYLE, $ex -bor $WS_EX_LAYERED -bor $WS_EX_TRANSPARENT)
        [void]$api::SetLayeredWindowAttributes($h, 0, [byte]$GhostAlpha, $LWA_ALPHA)
        # Full work area, so it cannot be occluded by anything.
        [void]$api::SetWindowPos($h, $HWND_TOPMOST, $scr.X, $scr.Y, $scr.Width, $scr.Height,
                                 $SWP_NOACTIVATE)
        Write-Host ("mstsc pid {0} -> GHOST {1}x{2} (invisible, click-through, topmost)" -f `
                    $p.Id, $scr.Width, $scr.Height) -ForegroundColor Green
    }
    Write-Host "  the client is now unoccludable but invisible -- the panel cannot freeze" -ForegroundColor Cyan
    Write-Host "  undo with:  .\minify-mstsc.ps1 -Process $Process -Restore" -ForegroundColor DarkGray
    return
}

# TOPMOST, so nothing can cover it.
#
# This is the whole reason the panel freezes. An RDP client that is COVERED stops
# requesting screen updates, the seat's desktop stops being composed, Desktop
# Duplication sees nothing, and the panel holds its last frame -- while every
# process still looks healthy and Windows still reports the window as
# IsWindowVisible=True. Occlusion simply does not register as "not visible", so
# nothing detects it.
#
# Measured with both mstsc and SDL-FreeRDP, so it is not a quirk of one client.
# The maximized mirror view window covering the client is the usual culprit.
#
# Keeping the client topmost as a small thumbnail means it can never be covered,
# which lets the view window be maximized underneath it.
if ($TopMost) {
    foreach ($p in $procs) {
        [void]$api::SetWindowPos($p.MainWindowHandle, $HWND_TOPMOST, 0, 0, 0, 0,
                                 $SWP_NOMOVE -bor $SWP_NOSIZE -bor $SWP_NOACTIVATE)
    }
    Write-Host "  pinned topmost -- nothing can cover it, so the panel cannot freeze" -ForegroundColor Green
}

Write-Host ""
Write-Host "Panel should keep updating. If it freezes, the window is minimized or" -ForegroundColor Cyan
Write-Host "off-screen -- both stop the RDP client requesting updates." -ForegroundColor Cyan
Write-Host "Restore with:  .\minify-mstsc.ps1 -Restore" -ForegroundColor Cyan
