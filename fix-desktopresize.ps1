#requires -Version 5.1
<#
    fix-desktopresize.ps1 -- PROBLEM 2, root cause found 2026-08-11.

    The /gfx crash was update->DesktopResize being NULL.

    gdi_graphics_pipeline_init sends ResetGraphics when the channel attaches,
    the server answers with a desktop-size notification, and libfreerdp calls
    update->DesktopResize(gdi->context) after checking only that `update`
    itself is non-NULL. Disassembly at libfreerdp3+0xDCB4B:

        test %r14,%r14            <- checks the struct
        je   ...
        mov  (%r12),%rcx          <- r12 = gdi, *gdi = gdi->context
        call *0x68(%r14)          <- does NOT check the slot

    offsetof(rdpUpdate, DesktopResize) == 0x068, one argument, returns BOOL.
    Exact match. Without /gfx that path never runs, which is why every non-gfx
    run was clean and eleven hypotheses about codecs, surfaces and pointers all
    missed it -- none of them looked at rdpUpdate.

    Run from C:\Programs\hydra. Backs up first, verifies every anchor before
    writing, refuses to touch anything if the source has drifted.
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

$anchorFn  = 'static BOOL hydra_post_connect(freerdp* instance)'
$anchorAsn = '    g_gdiBeginPaint = instance->context->update->BeginPaint;'

$t = [System.IO.File]::ReadAllText($Source)

if ($t -match 'hydra_desktop_resize') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    Select-String -Path $Source -Pattern 'hydra_desktop_resize|DesktopResize' |
        Select-Object LineNumber, Line | Format-Table -AutoSize
    return
}

$nFn  = ([regex]::Matches($t, [regex]::Escape($anchorFn))).Count
$nAsn = ([regex]::Matches($t, [regex]::Escape($anchorAsn))).Count

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  hydra_post_connect signature : {0}" -f $nFn)
Write-Host ("  g_gdiBeginPaint assignment   : {0}" -f $nAsn)
if ($nFn -ne 1 -or $nAsn -ne 1) {
    throw "expected exactly one of each anchor. Source has drifted -- read it before patching."
}

$handler = @'
/* PROBLEM 2, root cause found 2026-08-11.
 *
 * The /gfx crash was update->DesktopResize being NULL.
 * gdi_graphics_pipeline_init sends ResetGraphics on attach, the server answers
 * with a desktop-size notification, and libfreerdp calls
 * update->DesktopResize(gdi->context) having checked only that `update` itself
 * is non-NULL -- `call *0x68(%r14)`, one argument, offsetof confirmed 0x068.
 *
 * Without gfx that path never runs, which is why every non-gfx run was clean.
 * Eleven hypotheses -- codec, double channel init, pointer registration,
 * EndPaint chaining, channel interception, context layout, SoftwareGdi, thread
 * affinity, pipeline init stubs -- all missed it because none of them looked at
 * rdpUpdate. The VEH return address named the caller in one run. */
static BOOL hydra_desktop_resize(rdpContext* context)
{
    UINT32 w = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    UINT32 h = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    L("desktop resize -> %ux%u", w, h);
    if (!gdi_resize(context->gdi, w, h)) {
        L("gdi_resize failed");
        return FALSE;
    }
    return TRUE;
}

static BOOL hydra_post_connect(freerdp* instance)
'@

$t = $t.Replace($anchorFn, $handler)
$t = $t.Replace($anchorAsn,
    "    /* must be set BEFORE the gfx channel attaches -- see hydra_desktop_resize */`r`n" +
    "    instance->context->update->DesktopResize = hydra_desktop_resize;`r`n" +
    $anchorAsn)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'hydra_desktop_resize|DesktopResize' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Write-Host "test with:" -ForegroundColor Cyan
    Write-Host '  $env:HYDRA_GFX = ''RFX'''
    Write-Host '  .\dist\hydrardp.exe B teacher'
    Write-Host ""
    Write-Host "expect 'graphics pipeline attached' then 'desktop resize -> 1920x1080'" -ForegroundColor DarkGray
    Write-Host "where it used to die. Undo with: .\fix-desktopresize.ps1 -Revert" -ForegroundColor DarkGray
}
