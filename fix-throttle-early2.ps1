#requires -Version 5.1
<#
    fix-throttle-early2.ps1 -- as fix-throttle-early.ps1, but matches anchors
    with \r?\n so mixed line endings do not defeat it.

    hydrardp.c currently has 1048 CRLF and 44 bare LF, the latter introduced by
    the earlier patch scripts. Literal string matching therefore fails even
    though the source is exactly as expected.

    ---

    MEASURED 2026-08-11, mode 3 with /gfx:RFX during video playback:

        774 paints -> 11,162 paints per interval over one run
        published pinned at ~160 per interval throughout
        CPU 692s -> 877s across ~90s wall clock  ==  ~200%, two cores

    The main loop calls hydra_end_paint every iteration by design ("publish on
    a TIMER, not on paint callbacks"). Under gfx the socket nearly always has
    data pending, so the 16ms WaitForMultipleObjects returns at once and the
    loop runs ~4,100x/second rather than ~60.

    hydra_end_paint does its dirty-region bookkeeping BEFORE reaching the 16ms
    throttle, so ~98% of those calls do the work and discard it.

    THIS PATCH tests the clock first. A non-due call costs one GetTickCount and
    a compare. Safe because nothing above the old throttle position has side
    effects that must happen per call -- the g_gdiEndPaint chain is NULL under
    gfx (the attach log printed "chaining to gdi 0000000000000000"), and on the
    non-gfx path it still runs, just at the rate the frame was going to be
    published at anyway.

    If the picture gets WORSE -- stale or partial frames -- the chain mattered
    on a path not accounted for. -Revert and say so.
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

# Turn a literal block into a regex that accepts either line ending.
function New-AnchorPattern([string[]] $lines) {
    ($lines | ForEach-Object { [regex]::Escape($_) }) -join '\r?\n'
}

$entryLines = @(
    '    HydraContext* h = (HydraContext*)context;'
    '    rdpGdi* gdi = context->gdi;'
    '    if (!gdi) return TRUE;'
)
$thrLines = @(
    '    DWORD nowTick = GetTickCount();'
    '    BOOL  due = (nowTick - h->lastPublish) >= 16;'
    ''
    '    if (due && hydra_open_pixels(h)) {'
)

$entryPat = New-AnchorPattern $entryLines
$thrPat   = New-AnchorPattern $thrLines

$mEntry = [regex]::Matches($t, $entryPat)
$mThr   = [regex]::Matches($t, $thrPat)

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  function entry    : {0}" -f $mEntry.Count)
Write-Host ("  throttle block    : {0}" -f $mThr.Count)
if ($mEntry.Count -ne 1 -or $mThr.Count -ne 1) {
    throw "expected exactly one of each. Source has drifted -- read it before patching."
}

# Build replacements with CRLF explicitly, matching the file's majority.
$nl = "`r`n"

$entryNew = @(
    '    HydraContext* h = (HydraContext*)context;'
    '    rdpGdi* gdi = context->gdi;'
    '    if (!gdi) return TRUE;'
    ''
    '    /* EARLY THROTTLE.'
    '     *'
    '     * The main loop calls this every iteration on purpose (publish on a'
    '     * timer, not on paint callbacks). Under /gfx the socket nearly always'
    '     * has data pending, so the 16ms WaitForMultipleObjects returns at once'
    '     * and the loop runs ~4,100x/second instead of ~60. Measured 2026-08-11:'
    '     * paints climbed from 774 to 11,162 per interval while published stayed'
    '     * flat at ~160, and the process sat at roughly 200% CPU.'
    '     *'
    '     * Testing the clock BEFORE the region bookkeeping turns 98% of those'
    '     * calls into a tick compare. Nothing above the old throttle position'
    '     * had side effects that needed to happen per call. */'
    '    {'
    '        DWORD tick = GetTickCount();'
    '        if ((tick - h->lastPublish) < 16) return TRUE;'
    '    }'
) -join $nl

$thrNew = @(
    '    /* The early return at the top of this function already established that'
    '     * we are due. Recompute the tick so lastPublish records when the publish'
    '     * actually started rather than when the call arrived. */'
    '    DWORD nowTick = GetTickCount();'
    '    BOOL  due = TRUE;'
    ''
    '    if (due && hydra_open_pixels(h)) {'
) -join $nl

$t = [regex]::Replace($t, $entryPat, { $entryNew }, 1)
$t = [regex]::Replace($t, $thrPat,   { $thrNew },   1)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'EARLY THROTTLE|lastPublish|BOOL  due' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Write-Host "test:  cd C:\Programs\hydra; `$env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "published must stay ~160/interval. If it drops, the early return" -ForegroundColor DarkGray
    Write-Host "is firing when it should not -- revert." -ForegroundColor DarkGray
    Write-Host "CPU should fall well below the ~200% measured before:" -ForegroundColor DarkGray
    Write-Host "  Get-Process hydrardp | Select-Object CPU" -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-throttle-early2.ps1 -Revert" -ForegroundColor DarkGray
}
