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
    [switch]$Restore
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HydraWin {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr after, int X, int Y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
}
"@
Add-Type -AssemblyName System.Windows.Forms

$SWP_NOZORDER   = 0x0004
$SWP_NOACTIVATE = 0x0010
$SW_RESTORE     = 9

$procs = Get-Process mstsc -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 }
if (-not $procs) { Write-Warning "No mstsc window found -- is teacher's session connected?"; exit 1 }

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
$margin = 60
$maxW = [Math]::Max(320, $scr.Width  - $margin)
$maxH = [Math]::Max(200, $scr.Height - $margin)
if ($Width  -gt $maxW) { Write-Host "clamping width $Width -> $maxW"   -ForegroundColor DarkYellow; $Width  = $maxW }
if ($Height -gt $maxH) { Write-Host "clamping height $Height -> $maxH" -ForegroundColor DarkYellow; $Height = $maxH }

foreach ($p in $procs) {
    $h = $p.MainWindowHandle
    if ([HydraWin]::IsIconic($h)) {
        [void][HydraWin]::ShowWindow($h, $SW_RESTORE)   # minimized = frozen panel
        Start-Sleep -Milliseconds 300
    }

    if ($Fill) {
        # Fill the laptop's work area. Deliberately NOT a real maximize/fullscreen:
        # maximizing can land the window on the seat panel, where it would sit on
        # top of mirror's output and hide teacher's desktop. Sizing it explicitly
        # to the laptop screen keeps the panel clear, and the window still counts
        # as visibly composited so the RDP client keeps pulling frames.
        # Deliberately a margin short of the full work area -- filling it exactly
        # is what triggers fullscreen.
        [void][HydraWin]::SetWindowPos($h, [IntPtr]::Zero,
                                       $scr.X + 10, $scr.Y + 10, $maxW, $maxH,
                                       $SWP_NOZORDER -bor $SWP_NOACTIVATE)
        Write-Host ("mstsc pid {0} -> {1}x{2} at {3},{4} (large, still windowed)" -f `
                    $p.Id,$maxW,$maxH,($scr.X+10),($scr.Y+10)) -ForegroundColor Green
        continue
    }

    if ($Restore) {
        $w = [Math]::Min(1280, $maxW); $ht = [Math]::Min(800, $maxH)
        $x = $scr.X + [int](($scr.Width  - $w)  / 2)
        $y = $scr.Y + [int](($scr.Height - $ht) / 2)
        [void][HydraWin]::SetWindowPos($h, [IntPtr]::Zero, $x, $y, $w, $ht,
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
    [void][HydraWin]::SetWindowPos($h, [IntPtr]::Zero, $x, $y, $Width, $Height,
                                   $SWP_NOZORDER -bor $SWP_NOACTIVATE)
    Write-Host ("mstsc pid {0} -> {1}x{2} at {3},{4} ({5})" -f $p.Id,$Width,$Height,$x,$y,$Corner) -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Panel should keep updating. If it freezes, the window is minimized or" -ForegroundColor Cyan
Write-Host "off-screen -- both stop the RDP client requesting updates." -ForegroundColor Cyan
Write-Host "Restore with:  .\minify-mstsc.ps1 -Restore" -ForegroundColor Cyan
