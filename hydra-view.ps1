# hydra-view.ps1 -- bring up the whole seat, view window included, in one command.
#
# WHY THIS EXISTS
#   hydra-start.ps1 does the service, the client and the panel, but the view
#   window was left as a manual step -- and it MUST be last. Started before the
#   pixel ring has frames, a mirror comes up at ~14 MB and shows nothing: a black
#   or garbled window that looks like a rendering bug and is really a startup
#   ordering mistake. That has now cost several restarts, so the ordering belongs
#   in code rather than in a list of instructions.
#
#   Order is: service -> capture publishing -> panel -> view window. Each step is
#   verified before the next begins.
#
# USAGE:
#   .\hydra-view.ps1                       # everything
#   .\hydra-view.ps1 -NoView               # panel only
#   .\hydra-view.ps1 -Client mstsc         # fall back to the Microsoft client
#
# The view window opens BORDERLESS FULLSCREEN. F11 toggles to a resizable
# window and back; Alt-Tab works either way.

param(
    [ValidateSet('mstsc','freerdp')]
    [string]$Client   = 'freerdp',
    [string]$Seat     = 'B',
    [string]$Monitor  = '\\.\DISPLAY2',
    [int]$Port        = 56789,
    [string]$ViewSize = '1600x900',
    [switch]$NoView,
    [int]$TimeoutSec  = 90
)

$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$mirror = Join-Path $root 'dist\mirror.exe'
$ctl    = Join-Path $root 'dist\hydractl.exe'
$start  = Join-Path $root 'hydra-start.ps1'

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

foreach ($f in @($mirror, $ctl, $start)) {
    if (-not (Test-Path $f)) { throw "missing: $f  (run .\build.ps1)" }
}

# --- 1. everything except the view window ---------------------------------
Say "=== starting the seat ===" 'Cyan'
& $start -Client $Client
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { Say "hydra-start reported a problem" 'Yellow' }

# --- 2. is the panel actually alive? --------------------------------------
# A mirror that never received a frame sits around 14 MB; a presenting one is
# 70-98 MB because it has allocated D3D resources. That number is the most
# reliable signal we have, and it is worth checking rather than assuming.
Say ""
Say "verifying the panel before opening the view window..." 'Cyan'

$deadline = (Get-Date).AddSeconds(30)
$panelOk  = $false
while ((Get-Date) -lt $deadline) {
    $m = @(Get-Process mirror -ErrorAction SilentlyContinue |
           Where-Object { $_.WorkingSet64 -gt 40MB })
    if ($m.Count -ge 1) { $panelOk = $true; break }
    Start-Sleep -Seconds 2
}

if (-not $panelOk) {
    Say "the panel is not presenting -- NOT opening the view window." 'Red'
    Say "Opening it now would just produce a black window and hide the real" 'Red'
    Say "problem. Check:" 'Red'
    & $ctl status
    Say ""
    Say "  capture:$Seat missing or waiting -> the seat's session is not up" 'Yellow'
    Say "  service not reachable            -> Start-Service Hydra" 'Yellow'
    return
}
Say "panel is live" 'Green'

# --- 3. the view window, last ---------------------------------------------
if ($NoView) { Say "view window skipped (-NoView)" 'DarkGray'; return }

# Replace any earlier view window rather than stacking them: two readers are
# harmless, but a stale one from a previous run is usually the ~14 MB corpse
# that caused the confusion in the first place.
Get-Process mirror -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -eq 'Hydra - seat view' -or $_.WorkingSet64 -lt 40MB } |
    Stop-Process -Force -ErrorAction SilentlyContinue

Say "opening the view window..." 'Cyan'
Start-Process $mirror -ArgumentList $Seat, '--window', $ViewSize, "$Port"
Start-Sleep -Seconds 3

$all = @(Get-Process mirror -ErrorAction SilentlyContinue)
$good = @($all | Where-Object { $_.WorkingSet64 -gt 40MB })

Write-Host ""
if ($good.Count -ge 2) {
    Say "READY -- panel and view window both presenting" 'Green'
} else {
    Say "view window came up but is not presenting ($($good.Count) of $($all.Count) healthy)" 'Yellow'
    Say "re-run this script; it is almost always a startup race." 'Yellow'
}

$all | Select-Object Id, MainWindowTitle, @{n='MB';e={[int]($_.WorkingSet64/1MB)}} | Format-Table -AutoSize

Say "View window: F11 toggles fullscreen <-> window. Alt-Tab works either way." 'Cyan'
Say "Leave the RDP client thumbnail alone -- it only holds the session open." 'Cyan'
