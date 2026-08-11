#requires -Version 5.1
<#
    fix-endpaint.ps1 -- undo the last two /gfx bisects.

    BACKGROUND
    The /gfx crash was update->DesktopResize being NULL (found and fixed
    2026-08-11). Everything guarded by HYDRA_GFX in hydrardp.c was bisect
    scaffolding hunting that crash in the wrong place. Two guards remain:

      1. hydra_post_connect (~714-718) does not install hydra_end_paint when
         gfx is on, because gdi installs its own when the CHANNEL connects --
         which happens AFTER post_connect returns, so anything assigned there
         would be overwritten anyway. The log line says as much.

      2. hydra_end_paint (~237-238) does not chain to the saved gdi EndPaint
         when gfx is on.

    These are a pair and must move together. Installing the override without
    chaining means gdi's surface-to-primary_buffer step never runs and stale
    frames get published -- exactly what the comment at ~235 warns about.

    THIS PATCH
      - installs hydra_end_paint at the correct point: immediately after
        gdi_graphics_pipeline_init_ex succeeds (~621), capturing whatever gdi
        just installed into g_gdiEndPaint so the chain is intact
      - drops the gfx guard on chaining, so hydra_end_paint always calls
        through to gdi first
      - leaves the post_connect block alone for the non-gfx path, which still
        works exactly as before

    -Revert restores the newest backup.
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

if ($t -match 'g_gdiEndPaint = ctx->update->EndPaint') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

# --- anchor 1: the chaining guard in hydra_end_paint ------------------------
$chainPat = '(?s)\{ char gv2\[8\].*?return FALSE; \}'
$mChain = [regex]::Matches($t, $chainPat)

# --- anchor 2: the attach log line -----------------------------------------
$attachAnchor = '        L("graphics pipeline attached -- video should decode properly now");'
$mAttach = [regex]::Matches($t, [regex]::Escape($attachAnchor))

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  chaining guard block  : {0}" -f $mChain.Count)
Write-Host ("  attach log line       : {0}" -f $mAttach.Count)
if ($mChain.Count -ne 1 -or $mAttach.Count -ne 1) {
    throw "expected exactly one of each. Source has drifted -- read it before patching."
}

Write-Host ""
Write-Host "replacing chaining guard:" -ForegroundColor DarkGray
Write-Host $mChain[0].Value -ForegroundColor DarkGray
Write-Host ""

# 1. always chain. gdi's EndPaint is what moves surface data into
#    primary_buffer under gfx, so skipping it publishes stale pixels.
$chainNew = @'
/* Always chain. Under gfx this is gdi's own EndPaint, captured when the
     * channel attached; it is what moves surface data into primary_buffer, so
     * copying before it runs would publish a stale frame. The HYDRA_GFX guard
     * that used to sit here was bisect scaffolding for a crash that turned out
     * to be update->DesktopResize. */
    if (g_gdiEndPaint && !g_gdiEndPaint(context)) return FALSE;
'@
$t = [regex]::Replace($t, $chainPat, { $chainNew })

# 2. install our EndPaint after the pipeline is up, chaining to gdi's.
$attachNew = $attachAnchor + @'


        /* INSTALL OUR EndPaint HERE, NOT IN post_connect.
         *
         * gdi_graphics_pipeline_init_ex has just replaced update->EndPaint with
         * its own. post_connect runs BEFORE the channel connects, so anything
         * assigned there is overwritten a moment later -- which is why the old
         * code did not bother and simply logged that fact. Capture gdi's and
         * chain to it. */
        g_gdiEndPaint = ctx->update->EndPaint;
        ctx->update->EndPaint = hydra_end_paint;
        L("EndPaint installed after gfx attach (chaining to gdi %p)", (void*)g_gdiEndPaint);
'@
$t = $t.Replace($attachAnchor, $attachNew)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'g_gdiEndPaint|EndPaint installed|HYDRA_GFX' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Write-Host "test:  cd C:\Programs\hydra; `$env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher" -ForegroundColor Cyan
    Write-Host "expect 'EndPaint installed after gfx attach' and paints/published climbing." -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-endpaint.ps1 -Revert" -ForegroundColor DarkGray
}
