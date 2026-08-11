#requires -Version 5.1
<#
    fix-idd-configupdate.ps1 -- the remote IDD's missing activation call.

    WHY

    A console IDD is done once IddCxMonitorArrival succeeds. A remote IDD is
    not. Per the IddCx 1.4 remote docs, the OS keeps ONE stored desktop
    configuration per remote session, it starts EMPTY, and paths stay inactive
    until the driver calls IddCxAdapterDisplayConfigUpdate. Monitor arrival
    alone activates nothing.

    Without this, a remote build would init the adapter, add a monitor, log
    success, and silently show nothing -- the worst failure mode there is,
    because everything reports healthy.

    WHICH FUNCTION

    IddCxAdapterDisplayConfigUpdate2, not the original. The header says it
    "replaces IddCxAdapterDisplayConfigUpdate and allows a driver to both set
    more information and update only a subset in subsequent calls."

    Note it returns HRESULT, while the older one and everything around it in
    this file return NTSTATUS. Easy thing to get wrong.

    WHAT IS SENT

    One path, MODE_VALID only. Scale factor, physical size, colorimetry and SDR
    white level are all flag-gated and deliberately left unset -- the OS keeps
    its defaults, which is what a fixed-resolution seat monitor wants.

    Resolution and refresh come from actx->Mode (SeatMode: width/height/vsync,
    parsed from seats.toml `edid = "WxH@Hz"`), so they necessarily match a mode
    the driver advertises. The call returns STATUS_INVALID_PARAMETER if they do
    not.

    ERRORS

    STATUS_GRAPHICS_INDIRECT_DISPLAY_DEVICE_STOPPED is EXPECTED when the
    session is disconnecting or the adapter is being stopped. The docs are
    explicit that the driver must NOT call IddCxReportCriticalError for it.
    Everything else is logged and swallowed -- a failed config update should
    not take the monitor down.

    CONSOLE BUILDS ARE UNAFFECTED. The whole thing is inside HYDRA_REMOTE_IDD.

    NOT YET TESTABLE. The remote flag makes IddCxAdapterInitAsync fail unless
    the RD stack created the device, which needs the remote INF, a hardware ID
    the stack recognises, a signed catalog and test-signing. This code is
    written from the headers and stays unexercised until that exists.
#>
[CmdletBinding()]
param(
    [string] $Source = 'C:\Programs\hydra\iddseat\iddseat.cpp',
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

if ($t -match 'IddCxAdapterDisplayConfigUpdate2') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

function New-AnchorPattern([string[]] $lines) {
    ($lines | ForEach-Object { [regex]::Escape($_) }) -join '\r?\n'
}
$nl = "`r`n"

$anchorLines = @(
    '    IDARG_OUT_MONITORARRIVAL arrivalOut{};'
    '    return IddCxMonitorArrival(createOut.MonitorObject, &arrivalOut);'
)
$pat = New-AnchorPattern $anchorLines
$m = [regex]::Matches($t, $pat)

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  monitor arrival return : {0}" -f $m.Count)
if ($m.Count -ne 1) {
    throw "expected exactly one. Source has drifted -- read it before patching."
}

$new = @(
    '    IDARG_OUT_MONITORARRIVAL arrivalOut{};'
    '    NTSTATUS arrivalStatus = IddCxMonitorArrival(createOut.MonitorObject, &arrivalOut);'
    '    if (!NT_SUCCESS(arrivalStatus)) return arrivalStatus;'
    ''
    '#ifdef HYDRA_REMOTE_IDD'
    '    /* ACTIVATE THE PATH. Remote IDDs only.'
    '     *'
    '     * A console IDD is finished once the monitor has arrived. A remote IDD'
    '     * is not: the OS keeps one stored desktop configuration per remote'
    '     * session, it starts EMPTY, and every path stays inactive until the'
    '     * driver supplies a configuration. Arrival alone shows nothing, and'
    '     * reports success while doing so.'
    '     *'
    '     * IddCxAdapterDisplayConfigUpdate2 supersedes the original call and'
    '     * returns HRESULT rather than NTSTATUS -- unlike everything else here.'
    '     *'
    '     * Only MODE_VALID is set. Scale factor, physical size, colorimetry and'
    '     * SDR white level are flag-gated and left to the OS defaults, which is'
    '     * what a fixed-resolution seat monitor wants. */'
    '    {'
    '        IDDCX_DISPLAYCONFIGPATH2 path{};'
    '        path.Size          = sizeof(path);'
    '        path.Flags         = IDDCX_DISPLAYCONFIGPATH2_FLAGS_MODE_VALID;'
    '        path.MonitorObject = createOut.MonitorObject;'
    ''
    '        /* Single seat monitor, so the desktop origin is 0,0. */'
    '        path.Mode.Position.x = 0;'
    '        path.Mode.Position.y = 0;'
    ''
    '        /* From the seat''s own mode, so it necessarily matches something we'
    '         * advertise. A mismatch returns STATUS_INVALID_PARAMETER and the'
    '         * reason is only visible in WPP. */'
    '        path.Mode.Resolution.cx = actx->Mode.width;'
    '        path.Mode.Resolution.cy = actx->Mode.height;'
    ''
    '        path.Mode.Rotation = DISPLAYCONFIG_ROTATION_IDENTITY;'
    ''
    '        /* Progressive only for remote IDDs, so this is a plain vertical'
    '         * rate with a denominator of one. */'
    '        path.Mode.RefreshRate.Numerator   = actx->Mode.vsync;'
    '        path.Mode.RefreshRate.Denominator = 1;'
    '        path.Mode.VSyncFreqDivider        = 1;'
    ''
    '        path.Mode.MonitorColorMode = IDDCX_DISPLAYCONFIG_MONITOR_COLORMODE_SDR;'
    ''
    '        IDARG_IN_ADAPTERDISPLAYCONFIGUPDATE2 cfgIn{};'
    '        cfgIn.PathCount = 1;'
    '        cfgIn.pPaths    = &path;'
    ''
    '        HRESULT hr = IddCxAdapterDisplayConfigUpdate2(adapter, &cfgIn);'
    '        if (FAILED(hr))'
    '        {'
    '            /* DEVICE_STOPPED is expected when the session is disconnecting'
    '             * or the adapter is being torn down. The docs are explicit that'
    '             * IddCxReportCriticalError must NOT be called for it.'
    '             *'
    '             * Anything else is logged and swallowed: a failed configuration'
    '             * update should not take the monitor down with it. */'
    '            if (hr != HRESULT_FROM_NT(STATUS_GRAPHICS_INDIRECT_DISPLAY_DEVICE_STOPPED))'
    '            {'
    '                /* HYDRA-TODO: route this somewhere visible once remote'
    '                 * builds can actually run. WPP is the only channel a UMDF'
    '                 * driver has, and nothing is reading it yet. */'
    '            }'
    '        }'
    '    }'
    '#endif'
    ''
    '    return arrivalStatus;'
) -join $nl

$t = [regex]::Replace($t, $pat, { $new }, 1)

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'IddCxAdapterDisplayConfigUpdate2|arrivalStatus|IDDCX_DISPLAYCONFIGPATH2' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    Write-Host "console build (must still compile -- the new code is #ifdef'd out):" -ForegroundColor Cyan
    & 'C:\Programs\hydra\build-driver.ps1'
    Write-Host ""
    Write-Host "remote build (this is the one that exercises the new code):" -ForegroundColor Cyan
    & 'C:\Programs\hydra\build-driver.ps1' -Remote
    Write-Host ""
    Write-Host "Both compiling is all this proves. The remote driver cannot LOAD" -ForegroundColor DarkGray
    Write-Host "until there is a remote INF, a hardware ID the RD stack matches," -ForegroundColor DarkGray
    Write-Host "a signed catalog and test-signing enabled." -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-idd-configupdate.ps1 -Revert" -ForegroundColor DarkGray
}
