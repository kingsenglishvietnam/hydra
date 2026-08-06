# client-watchdog.ps1 -- keep the RDP client un-minimized, forever.
#
# THE PROBLEM
#   A minimized RDP client stops requesting screen updates. The seat's desktop
#   then stops being composed, Desktop Duplication sees nothing, and the panel
#   freezes on its last frame -- while every process still looks healthy.
#
#   Two distinct causes were measured, and only one is detectable:
#     MINIMIZED  -- IsIconic() reports it. This script fixes that.
#     OCCLUDED   -- IsWindowVisible() still reports True, so nothing detects it.
#                   The only defence is keeping the client unoccluded (topmost).
#
#   A 1x1 topmost window and a full-size transparent one were both tried as ways
#   to be "visible but invisible": both froze the panel. The client needs real,
#   visible, uncovered area.
#
# USAGE (leave it running in a background window, or via Start-Process):
#   .\client-watchdog.ps1                       # watches mstsc
#   .\client-watchdog.ps1 -Process sdl-freerdp
#   .\client-watchdog.ps1 -Restore              # also re-assert topmost each time

param(
    [string]$Process   = 'mstsc',
    [int]$IntervalSec  = 5,
    [switch]$TopMost           # re-pin topmost as well as restoring
)

$ErrorActionPreference = 'Continue'

$tn = "HydraWd_" + [guid]::NewGuid().ToString("N")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class $tn {
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr after, int X, int Y, int cx, int cy, uint flags);
}
"@ | Out-Null
$api = [type]$tn

$SW_RESTORE     = 9
$HWND_TOPMOST   = [IntPtr]-1
$SWP_NOMOVE     = 0x0002
$SWP_NOSIZE     = 0x0001
$SWP_NOACTIVATE = 0x0010

Write-Host "watching $Process every ${IntervalSec}s -- restoring it if minimized" -ForegroundColor Cyan
Write-Host "Ctrl+C to stop." -ForegroundColor DarkGray

$restores = 0
while ($true) {
    try {
        $p = Get-Process $Process -ErrorAction SilentlyContinue |
             Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($p) {
            $h = $p.MainWindowHandle
            if ($api::IsIconic($h)) {
                # SW_RESTORE deliberately, not SW_SHOW: restore returns it to its
                # previous size and position, which is where it was placed. It
                # also does NOT steal focus the way activating would.
                [void]$api::ShowWindow($h, $SW_RESTORE)
                $restores++
                Write-Host ("[{0}] {1} was minimized -- restored (#{2})" -f `
                            (Get-Date -Format HH:mm:ss), $Process, $restores) -ForegroundColor Yellow
            }
            if ($TopMost) {
                [void]$api::SetWindowPos($h, $HWND_TOPMOST, 0, 0, 0, 0,
                                         $SWP_NOMOVE -bor $SWP_NOSIZE -bor $SWP_NOACTIVATE)
            }
        }
    } catch { }
    Start-Sleep -Seconds $IntervalSec
}
