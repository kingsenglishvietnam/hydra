#requires -Version 5.1
<#
    fix-gfx-signal.ps1 -- publish when gfx says the surface is ready, not on a
                          timer that has no relationship to the blit.

    THE PROBLEM (hydrardp.c's own comment at ~line 1069 states it)

        "With the graphics pipeline, content arrives as SURFACES that gfx blits
         to the output on its own schedule -- EndPaint is not the signal."

    The response to that was to sample gdi->primary_buffer every 16ms from the
    main loop. But a 60Hz timer is no more the signal than EndPaint was: it
    fires whenever it fires, which can be mid-blit or between blits. Copying
    mid-blit publishes a frame that is part new and part old -- torn, partial,
    "glitchy" -- regardless of codec, CPU or throttle rate.

    Measured 2026-08-11 after the CPU spin was fixed: 1,878,812 paints against
    39,723 published, CPU down to normal, picture unchanged. CPU was never the
    cause.

    THE SIGNAL THAT DOES EXIST

    hydra_gfx_update_surface_area() -- already wired into
    gdi_graphics_pipeline_init_ex at ~line 613, currently a no-op whose comment
    reads "The decoded pixels are already in gdi->primary_buffer by the time
    this is called". That is precisely the moment to publish, and it was
    written down and then not used.

    THIS PATCH
      - makes hydra_gfx_update_surface_area publish, by calling hydra_end_paint
      - drops the unconditional per-iteration hydra_end_paint from the main
        loop when gfx is on, since the callback now drives it
      - leaves the cursor-moved republish alone: the pointer moves independently
        of surface updates and still needs its own nudge
      - leaves the non-gfx path completely unchanged

    The 16ms throttle inside hydra_end_paint still applies, so a burst of
    surface updates cannot flood the ring.

    IF THE PICTURE GOES BLACK OR FREEZES, the callback is not firing as often as
    assumed -- some servers send few surface-area updates and rely on frame
    markers instead. -Revert, and the next thing to try is StartFrame/EndFrame
    on the gfx context rather than surface area.
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

if ($t -match 'GFX IS THE SIGNAL') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

function New-AnchorPattern([string[]] $lines) {
    ($lines | ForEach-Object { [regex]::Escape($_) }) -join '\r?\n'
}

$nl = "`r`n"

# --- anchor 1: the no-op surface-area callback body ------------------------
$cbLines = @(
    '    /* The decoded pixels are already in gdi->primary_buffer by the time this is'
    '     * called; our EndPaint publishes the whole frame from there. Nothing to do'
    '     * per-rectangle. */'
    '    (void)context; (void)surfaceId; (void)nrRects; (void)rects;'
    '    return CHANNEL_RC_OK;'
)

# --- anchor 2: the unconditional publish in the main loop ------------------
$loopLines = @(
    '        hydra_end_paint(inst->context);'
)

$cbPat   = New-AnchorPattern $cbLines
$loopPat = New-AnchorPattern $loopLines

$mCb   = [regex]::Matches($t, $cbPat)
$mLoop = [regex]::Matches($t, $loopPat)

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  surface-area callback body : {0}" -f $mCb.Count)
Write-Host ("  main-loop publish call     : {0}" -f $mLoop.Count)
if ($mCb.Count -ne 1) {
    throw "expected exactly one surface-area callback body. Source has drifted."
}
if ($mLoop.Count -lt 1) {
    throw "main-loop publish call not found. Source has drifted."
}

# The cursor-moved branch also contains hydra_end_paint but on the SAME line as
# its if(), so the bare-line anchor above matches only the standalone call.
if ($mLoop.Count -ne 1) {
    throw "expected exactly one standalone main-loop publish, found $($mLoop.Count). Read the loop before patching."
}

$cbNew = @(
    '    /* GFX IS THE SIGNAL.'
    '     *'
    '     * The decoded pixels are in gdi->primary_buffer by the time this fires,'
    '     * so this is the correct moment to publish -- and the only one. The main'
    '     * loop previously sampled every 16ms instead, but a timer bears no'
    '     * relationship to when gfx finishes a blit: it can land mid-blit and'
    '     * publish a frame that is part new and part old. That is what "glitchy"'
    '     * was, and it survived the codec change, the CPU fix and the throttle'
    '     * move because none of them touched WHEN the sample is taken.'
    '     *'
    '     * The 16ms throttle inside hydra_end_paint still applies, so a burst of'
    '     * surface updates cannot flood the ring. */'
    '    (void)surfaceId; (void)nrRects; (void)rects;'
    '    if (context && context->custom) {'
    '        rdpGdi* g = (rdpGdi*)context->custom;'
    '        if (g && g->context) hydra_end_paint(g->context);'
    '    }'
    '    return CHANNEL_RC_OK;'
) -join $nl

$loopNew = @(
    '        /* Under gfx, hydra_gfx_update_surface_area publishes instead -- it'
    '         * fires when the pixels are actually in primary_buffer, which a timer'
    '         * cannot know. Sampling here as well would reintroduce the mid-blit'
    '         * frames this was changed to avoid. */'
    '        if (!g_gfxOn) hydra_end_paint(inst->context);'
) -join $nl

$t = [regex]::Replace($t, $cbPat,   { $cbNew },   1)
$t = [regex]::Replace($t, $loopPat, { $loopNew }, 1)

# --- add the g_gfxOn flag, set where the pipeline attaches -----------------
$flagAnchorLines = @(
    'static pBeginPaint g_gdiBeginPaint = NULL;'
)
$flagPat = New-AnchorPattern $flagAnchorLines
if (([regex]::Matches($t, $flagPat)).Count -ne 1) {
    throw "g_gdiBeginPaint declaration not found exactly once. Source has drifted."
}
$flagNew = @(
    'static BOOL g_gfxOn = FALSE;   /* set when the gfx channel attaches; the'
    '                                * surface-area callback then owns publishing */'
    'static pBeginPaint g_gdiBeginPaint = NULL;'
) -join $nl
$t = [regex]::Replace($t, $flagPat, { $flagNew }, 1)

$attachLines = @(
    '        L("graphics pipeline attached -- video should decode properly now");'
)
$attachPat = New-AnchorPattern $attachLines
if (([regex]::Matches($t, $attachPat)).Count -ne 1) {
    throw "attach log line not found exactly once. Source has drifted."
}
$attachNew = @(
    '        g_gfxOn = TRUE;'
    '        L("graphics pipeline attached -- video should decode properly now");'
) -join $nl
$t = [regex]::Replace($t, $attachPat, { $attachNew }, 1)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'GFX IS THE SIGNAL|g_gfxOn' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    Stop-Process -Name hydrardp -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Get-Item 'C:\Programs\hydra\rdp\hydrardp.c','C:\Programs\hydra\dist\hydrardp.exe' |
        Select-Object Name, LastWriteTime | Format-Table -AutoSize
    Write-Host "exe must be NEWER than the .c above." -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "test:  cd C:\Programs\hydra; `$env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "paints should now be MUCH lower -- it counts publish attempts, and" -ForegroundColor DarkGray
    Write-Host "they are now driven by surface updates rather than a spinning loop." -ForegroundColor DarkGray
    Write-Host "If the picture freezes or goes black, the callback fires too rarely:" -ForegroundColor DarkGray
    Write-Host "  .\fix-gfx-signal.ps1 -Revert" -ForegroundColor DarkGray
}
