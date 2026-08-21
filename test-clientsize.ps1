# test-clientsize.ps1 -- find the smallest RDP client window that keeps the panel alive.
#
# WHY THIS EXISTS
#   A COVERED RDP client stops requesting screen updates. The seat's desktop then
#   stops being composed, Desktop Duplication sees nothing, and the panel freezes
#   -- while Windows still reports the window as visible, so nothing detects it.
#
#   The client therefore has to keep some visible, uncovered area. How much was
#   being guessed at by shrinking the window and staring at a monitor for
#   minutes, which is slow and easy to get wrong: a 1x1 window and a full-size
#   transparent one both appeared to fail, but those runs were tangled up with
#   other changes.
#
#   This measures it instead, using mirror's probe mode to count frames.
#
# IMPORTANT: the seat's desktop must have something CHANGING on it -- a clock
# with a second hand, a blinking caret, a video. Desktop Duplication publishes on
# change, so an idle desktop legitimately produces no frames and every size would
# read as frozen.
#
# USAGE (with Hydra running and the view window covering the client):
#   .\test-clientsize.ps1
#   .\test-clientsize.ps1 -Process sdl-freerdp -Seconds 15

param(
    [string]$Process = 'mstsc',
    [string]$Seat    = 'B',
    [int]$Seconds    = 10,
    [int[]]$Sizes    = @(320, 160, 80, 40, 20, 8, 4, 2, 1, 0)
)

$ErrorActionPreference = 'Continue'
# PS 7.4 made native-command stderr honour ErrorActionPreference. Several tools
# here write PROGRESS to stderr -- hydractl's 'not reachable' while it waits,
# mirror's 'pixel transport opened' -- and 2>&1 under 'Stop' turned those
# SUCCESS lines into terminating errors. This broke hydra-start.ps1 on 2026-08-21.
$PSNativeCommandUseErrorActionPreference = $false
$minify = Join-Path $PSScriptRoot 'minify-mstsc.ps1'
$mirror = Join-Path $PSScriptRoot 'dist\mirror.exe'

foreach ($f in @($minify, $mirror)) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}
if (-not (Get-Process $Process -ErrorAction SilentlyContinue)) {
    throw "$Process is not running -- start the seat's session first"
}

Write-Host ""
Write-Host "Measuring the smallest client window that keeps seat $Seat alive." -ForegroundColor Cyan
Write-Host "Make sure something is CHANGING on the seat's desktop, and that the" -ForegroundColor Yellow
Write-Host "view window (or anything else) is COVERING the client." -ForegroundColor Yellow
Write-Host ""

$results = @()
foreach ($w in $Sizes) {
    $h = [Math]::Max(1, [int]($w * 0.625))   # keep roughly 16:10
    if ($w -eq 0) { $h = 0 }

    & $minify -Process $Process -TopMost -Width $w -Height $h -Corner TopRight | Out-Null
    Start-Sleep -Seconds 2      # let the client notice and resume

    $out = & $mirror $Seat --probe $Seconds 2>&1 | Out-String
    $alive = $LASTEXITCODE -eq 0
    $fps = if ($out -match '\(([\d.]+) fps\)') { $Matches[1] } else { '?' }

    $results += [pscustomobject]@{ Size = "${w}x${h}"; Fps = $fps; Alive = $alive }
    Write-Host ("  {0,-9} {1,6} fps  {2}" -f "${w}x${h}", $fps,
                $(if ($alive) { "ALIVE" } else { "frozen" })) `
               -ForegroundColor $(if ($alive) { 'Green' } else { 'DarkGray' })
}

Write-Host ""
$smallest = $results | Where-Object Alive | Select-Object -Last 1
if ($smallest) {
    Write-Host "Smallest size that stayed alive: $($smallest.Size)" -ForegroundColor Green
    Write-Host "Use something LARGER than that in practice -- a size that only just" -ForegroundColor Yellow
    Write-Host "clears the threshold is exactly what starts failing intermittently." -ForegroundColor Yellow
} else {
    Write-Host "Nothing stayed alive. Either the seat's desktop is idle (no frames to" -ForegroundColor Red
    Write-Host "publish regardless of size), or the client was not actually covered." -ForegroundColor Red
}
Write-Host ""
$results | Format-Table -AutoSize
