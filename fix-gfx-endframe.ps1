#requires -Version 5.1
<#
    fix-gfx-endframe.ps1 -- publish when gfx signals a completed FRAME.

    WHY, and what has already been ruled out

    Video is glitchy under /gfx even though the pipeline attaches, decodes and
    publishes at 60fps. Eliminated with measurements on 2026-08-11:

      codec          /gfx:RFX attaches and decodes; not it
      CPU            was ~200% from a spinning loop, now ~93s total; unchanged
      throttle       60fps publishing, 172 paints / 172 published / 0 coalesced
                     after the early-throttle fix; nothing is being dropped
      surface area   hydra_gfx_update_surface_area never fires on this server;
                     publishing from it froze the panel on frame 1 (reverted)

    What remains is WHEN the sample is taken. The main loop copies
    gdi->primary_buffer every 16ms on a timer with no relationship to when gfx
    finishes a blit, so it can land mid-blit and publish a frame that is part
    new and part old. hydrardp.c's own comment at ~line 1069 says as much:
    "content arrives as SURFACES that gfx blits to the output on its own
    schedule -- EndPaint is not the signal."

    A timer is not the signal either.

    THE SIGNAL

    RDPGFX_END_FRAME_PDU. The server sends it to mark a frame complete, and
    gdi_graphics_pipeline_init_ex fills gfx->EndFrame to handle it. Capture that
    pointer and chain to it, exactly as fix-endpaint.ps1 does for
    update->EndPaint -- but on the callback this server actually sends.

    pcRdpgfxEndFrame sits at offset 0x20 in s_rdpgfx_client_context
    (handle 0x00, custom 0x08, ResetGraphics 0x10, StartFrame 0x18,
    EndFrame 0x20).

    The 16ms throttle inside hydra_end_paint still applies, so a fast frame
    rate cannot flood the ring.

    IF THE PANEL FREEZES on frame 1, EndFrame is not firing either and the
    server is driving everything through SurfaceCommand. -Revert and say so;
    that is the next slot to try.
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

if ($t -match 'ENDFRAME IS THE SIGNAL') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

function New-AnchorPattern([string[]] $lines) {
    ($lines | ForEach-Object { [regex]::Escape($_) }) -join '\r?\n'
}
$nl = "`r`n"

# --- anchor 1: the g_gdiEndPaint declaration, for the new statics ----------
$declLines = @('static pBeginPaint g_gdiBeginPaint = NULL;')
$declPat = New-AnchorPattern $declLines

# --- anchor 2: the EndPaint install block at the attach point --------------
$installLines = @(
    '        g_gdiEndPaint = ctx->update->EndPaint;'
    '        ctx->update->EndPaint = hydra_end_paint;'
    '        L("EndPaint installed after gfx attach (chaining to gdi %p)", (void*)g_gdiEndPaint);'
)
$installPat = New-AnchorPattern $installLines

$mDecl    = [regex]::Matches($t, $declPat)
$mInstall = [regex]::Matches($t, $installPat)

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  g_gdiBeginPaint declaration : {0}" -f $mDecl.Count)
Write-Host ("  EndPaint install block      : {0}" -f $mInstall.Count)
if ($mDecl.Count -ne 1 -or $mInstall.Count -ne 1) {
    throw "expected exactly one of each. Source has drifted -- read it before patching."
}

# The handler must be declared before hydra_on_channel_connected uses it, and
# hydra_end_paint must already be visible to it. Placing the forward decl and
# the handler next to the existing statics satisfies both, since
# hydra_end_paint is defined well above the attach point.
$declNew = @(
    'static pBeginPaint g_gdiBeginPaint = NULL;'
    ''
    '/* ENDFRAME IS THE SIGNAL.'
    ' *'
    ' * RDPGFX_END_FRAME_PDU is what the server sends to mark a frame complete.'
    ' * gdi_graphics_pipeline_init_ex installs its own handler for it; we capture'
    ' * that, chain to it so gdi still does its bookkeeping, and publish after.'
    ' *'
    ' * The main loop used to sample primary_buffer on a blind 16ms timer, which'
    ' * can land mid-blit and publish a frame that is part new and part old --'
    ' * the glitching that survived the codec change, the CPU fix and the'
    ' * throttle move, because none of them touched WHEN the sample was taken. */'
    'static pcRdpgfxEndFrame g_gfxEndFrame = NULL;'
    'static BOOL hydra_end_paint(rdpContext* context);'
    ''
    'static UINT hydra_gfx_end_frame(RdpgfxClientContext* gfx,'
    '                                const RDPGFX_END_FRAME_PDU* endFrame)'
    '{'
    '    UINT rc = CHANNEL_RC_OK;'
    '    if (g_gfxEndFrame) rc = g_gfxEndFrame(gfx, endFrame);   /* gdi first */'
    '    if (gfx && gfx->custom) {'
    '        rdpGdi* g = (rdpGdi*)gfx->custom;'
    '        if (g && g->context) hydra_end_paint(g->context);'
    '    }'
    '    return rc;'
    '}'
) -join $nl

$installNew = @(
    '        g_gdiEndPaint = ctx->update->EndPaint;'
    '        ctx->update->EndPaint = hydra_end_paint;'
    '        L("EndPaint installed after gfx attach (chaining to gdi %p)", (void*)g_gdiEndPaint);'
    ''
    '        /* Publish on completed FRAMES rather than on a timer. See'
    '         * hydra_gfx_end_frame for why. */'
    '        {'
    '            RdpgfxClientContext* gfx = (RdpgfxClientContext*)e->pInterface;'
    '            if (gfx) {'
    '                g_gfxEndFrame  = gfx->EndFrame;'
    '                gfx->EndFrame  = hydra_gfx_end_frame;'
    '                L("gfx EndFrame installed (chaining to gdi %p)", (void*)g_gfxEndFrame);'
    '            }'
    '        }'
) -join $nl

$t = [regex]::Replace($t, $declPat,    { $declNew },    1)
$t = [regex]::Replace($t, $installPat, { $installNew }, 1)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'ENDFRAME IS THE SIGNAL|g_gfxEndFrame|hydra_gfx_end_frame' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    Stop-Process -Name hydrardp -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Get-Item 'C:\Programs\hydra\rdp\hydrardp.c','C:\Programs\hydra\dist\hydrardp.exe' |
        Select-Object Name, LastWriteTime | Format-Table -AutoSize
    Write-Host "the exe must be NEWER than the .c above, or the build did not land." -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "test:  cd C:\Programs\hydra; `$env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "look for 'gfx EndFrame installed (chaining to gdi ...)' -- a NON-null" -ForegroundColor DarkGray
    Write-Host "pointer there means gdi filled the slot and the chain is real." -ForegroundColor DarkGray
    Write-Host "The main loop still publishes on its timer as well, so a frozen panel" -ForegroundColor DarkGray
    Write-Host "is not a risk; if the picture is unchanged, EndFrame is not firing." -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-gfx-endframe.ps1 -Revert" -ForegroundColor DarkGray
}
