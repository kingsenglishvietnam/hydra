#requires -Version 5.1
<#
    fix-throttle-early.ps1 -- stop hydra_end_paint doing work it will throw away.

    MEASURED 2026-08-11, mode 3 with /gfx:RFX during video playback:

        774 paints -> 11,162 paints per interval over one run
        published pinned at ~160 per interval throughout
        CPU 692s -> 877s across ~90s wall clock  ==  ~200%, two cores

    The main loop calls hydra_end_paint every iteration by design (line ~1079:
    "Publish on a TIMER, not on paint callbacks"). Under gfx the socket almost
    always has data pending, so WaitForMultipleObjects returns immediately and
    the loop runs ~4,100 times a second rather than ~60.

    hydra_end_paint then does its dirty-region bookkeeping BEFORE reaching the
    16ms throttle at ~line 308, so ~98% of those calls do the work and discard
    it. Cheap per call; not cheap four thousand times a second on an i7-1065G7
    that is also decoding video.

    THIS PATCH tests the clock first. A non-due call costs one GetTickCount and
    a compare, then returns. Safe because nothing above the throttle has side
    effects that must happen per call:
      - the g_gdiEndPaint chain is NULL under gfx (verified: the attach log
        printed "chaining to gdi 0000000000000000"), and on the non-gfx path
        it is still called, just only on due ticks -- which is the same rate
        the frame was going to be published at anyway
      - the buffer guard and dirty-region tracking only feed the publish

    If the picture gets WORSE (stale or partial frames), the chain did matter
    on some path. -Revert and say so.
#>
[CmdletBinding()]
param(
    [string] $Source = 'C:\Programs\hydra\rdp\hydrardp.c',
    [switch] $Build,
    [switch] $Revert
)

$ErrorActionPreference = 'Stop'

if ($Revert) {
    $bak = Get-ChildItem "$Source.bak-*" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $bak) { throw "no backup found next to $Source" }
    Copy-Item $bak.FullName $Source -Force
    Write-Host "restored $($bak.Name)" -ForegroundColor Green
    return
}

if (-not (Test-Path $Source)) { throw "not found: $Source" }
$t = [System.IO.File]::ReadAllText($Source)

if ($t -match 'EARLY THROTTLE') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

# --- anchor 1: function entry, just after the gdi null check ---------------
$entryAnchor = @'
    HydraContext* h = (HydraContext*)context;
    rdpGdi* gdi = context->gdi;
    if (!gdi) return TRUE;
'@

# --- anchor 2: the existing throttle computation ---------------------------
$oldThrottle = @'
    DWORD nowTick = GetTickCount();
    BOOL  due = (nowTick - h->lastPublish) >= 16;

    if (due && hydra_open_pixels(h)) {
'@

$nEntry = ([regex]::Matches($t, [regex]::Escape($entryAnchor))).Count
$nThr   = ([regex]::Matches($t, [regex]::Escape($oldThrottle))).Count

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  function entry    : {0}" -f $nEntry)
Write-Host ("  throttle block    : {0}" -f $nThr)
if ($nEntry -ne 1 -or $nThr -ne 1) {
    throw "expected exactly one of each. Source has drifted -- read it before patching."
}

$entryNew = @'
    HydraContext* h = (HydraContext*)context;
    rdpGdi* gdi = context->gdi;
    if (!gdi) return TRUE;

    /* EARLY THROTTLE.
     *
     * The main loop calls this every iteration on purpose (publish on a timer,
     * not on paint callbacks). Under /gfx the socket nearly always has data
     * pending, so the 16ms WaitForMultipleObjects returns at once and the loop
     * runs ~4,100x/second instead of ~60. Measured 2026-08-11: paints climbed
     * from 774 to 11,162 per interval while published stayed flat at ~160, and
     * the process sat at ~200% CPU.
     *
     * Testing the clock BEFORE the region bookkeeping turns 98% of those calls
     * into a tick compare. Nothing above the old throttle position had side
     * effects that needed to happen per call. */
    {
        DWORD tick = GetTickCount();
        if ((tick - h->lastPublish) < 16) return TRUE;
    }
'@

$newThrottle = @'
    /* The early return above already established that we are due; recompute the
     * tick here so lastPublish records when the publish actually started. */
    DWORD nowTick = GetTickCount();
    BOOL  due = TRUE;

    if (due && hydra_open_pixels(h)) {
'@

$t = $t.Replace($entryAnchor, $entryNew)
$t = $t.Replace($oldThrottle, $newThrottle)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'EARLY THROTTLE|lastPublish' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Write-Host "test:  cd C:\Programs\hydra; `$env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "published should stay ~160/interval -- that part must NOT change." -ForegroundColor DarkGray
    Write-Host "CPU should drop well below the ~200% measured before." -ForegroundColor DarkGray
    Write-Host "check with:  Get-Process hydrardp | Select-Object CPU" -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-throttle-early.ps1 -Revert" -ForegroundColor DarkGray
}
