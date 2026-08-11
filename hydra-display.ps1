#requires -Version 5.1
#requires -RunAsAdministrator
<#
    hydra-display.ps1 — move a seat monitor so its edge no longer abuts the
    console monitor along a high-traffic border.

    PROBLEM 4, correctly diagnosed 2026-08-10: the cursor "leak" is not input
    forwarding and not a window at all. The seat panel sits on a real monitor
    (\\.\DISPLAY2 at 3240,0). Windows treats every display as one coordinate
    space, so at the console monitor's right edge the pointer simply continues
    onto the next adjacent pixels — which belong to the seat. No window is in
    the path, which is why repositioning sdl-freerdp and the mirror view both
    changed nothing.

    THE FIX IS GEOMETRY. Put the seat monitor ABOVE the console monitor rather
    than beside it, offset horizontally so only a narrow strip of the two edges
    actually touches. Windows only lets the pointer cross where edges overlap,
    so the crossing zone shrinks from a full 1080px vertical border to whatever
    -OverlapPx you choose, in a corner you never visit.

    Above rather than left: vertical overshoot is rarer than horizontal, and
    the top edge is already guarded by title bars, tab strips and menus. The
    left edge is where people throw the cursor for Start and window controls.

    The desktop must stay contiguous — Windows rejects an arrangement where a
    display touches nothing. Hence a deliberate overlap rather than a gap.

    Usage:
        .\hydra-display.ps1 -List
        .\hydra-display.ps1 -Device '\\.\DISPLAY2' -Above -OverlapPx 200
        .\hydra-display.ps1 -Device '\\.\DISPLAY2' -Above -Apply
        .\hydra-display.ps1 -Revert

    Without -Apply it prints the plan and changes nothing.
#>
[CmdletBinding(DefaultParameterSetName = 'List')]
param(
    [Parameter(ParameterSetName = 'List')]
    [switch] $List,

    [Parameter(ParameterSetName = 'Move', Mandatory = $true)]
    [string] $Device,

    [Parameter(ParameterSetName = 'Move')]
    [switch] $Above,

    # Horizontal contact strip left between the two monitors, in pixels.
    # Smaller = harder to cross. Too small and Windows may refuse the
    # arrangement as non-contiguous; 100–300 is the practical range.
    [Parameter(ParameterSetName = 'Move')]
    [int] $OverlapPx = 200,

    # Which end of the console monitor's top edge to leave touching.
    [Parameter(ParameterSetName = 'Move')]
    [ValidateSet('Left','Right')]
    [string] $Corner = 'Right',

    [Parameter(ParameterSetName = 'Move')]
    [switch] $Apply,

    [Parameter(ParameterSetName = 'Revert')]
    [switch] $Revert,

    [string] $BackupPath = 'C:\ProgramData\Hydra\display-backup.json'
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public ushort dmSpecVersion;
    public ushort dmDriverVersion;
    public ushort dmSize;
    public ushort dmDriverExtra;
    public uint   dmFields;
    public int    dmPositionX;
    public int    dmPositionY;
    public uint   dmDisplayOrientation;
    public uint   dmDisplayFixedOutput;
    public short  dmColor;
    public short  dmDuplex;
    public short  dmYResolution;
    public short  dmTTOption;
    public short  dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public ushort dmLogPixels;
    public uint   dmBitsPerPel;
    public uint   dmPelsWidth;
    public uint   dmPelsHeight;
    public uint   dmDisplayFlags;
    public uint   dmDisplayFrequency;
    public uint   dmICMMethod;
    public uint   dmICMIntent;
    public uint   dmMediaType;
    public uint   dmDitherType;
    public uint   dmReserved1;
    public uint   dmReserved2;
    public uint   dmPanningWidth;
    public uint   dmPanningHeight;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]  public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
    public uint StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
}

public static class Disp {
    public const uint DM_POSITION            = 0x00000020;
    public const int  ENUM_CURRENT_SETTINGS  = -1;
    public const uint CDS_UPDATEREGISTRY     = 0x00000001;
    public const uint CDS_NORESET            = 0x10000000;
    public const uint DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x00000001;
    public const uint DISPLAY_DEVICE_PRIMARY_DEVICE      = 0x00000004;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum,
                                                 ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettings(string lpszDeviceName, int iModeNum,
                                                  ref DEVMODE lpDevMode);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int ChangeDisplaySettingsEx(string lpszDeviceName, ref DEVMODE lpDevMode,
                                                     IntPtr hwnd, uint dwflags, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "ChangeDisplaySettingsExW")]
    public static extern int ApplyPending(string lpszDeviceName, IntPtr lpDevMode,
                                          IntPtr hwnd, uint dwflags, IntPtr lParam);
}
'@ -ErrorAction Stop

function Get-Displays {
    $out = @()
    for ($i = 0; $i -lt 16; $i++) {
        $dd = New-Object DISPLAY_DEVICE
        $dd.cb = [Runtime.InteropServices.Marshal]::SizeOf($dd)
        if (-not [Disp]::EnumDisplayDevices($null, $i, [ref]$dd, 0)) { break }
        if (($dd.StateFlags -band [Disp]::DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) -eq 0) { continue }

        $dm = New-Object DEVMODE
        $dm.dmSize = [uint16][Runtime.InteropServices.Marshal]::SizeOf($dm)
        if (-not [Disp]::EnumDisplaySettings($dd.DeviceName, [Disp]::ENUM_CURRENT_SETTINGS, [ref]$dm)) { continue }

        $out += [pscustomobject]@{
            Name    = $dd.DeviceName
            Model   = $dd.DeviceString
            Primary = (($dd.StateFlags -band [Disp]::DISPLAY_DEVICE_PRIMARY_DEVICE) -ne 0)
            X       = $dm.dmPositionX
            Y       = $dm.dmPositionY
            Width   = [int]$dm.dmPelsWidth
            Height  = [int]$dm.dmPelsHeight
            Hz      = [int]$dm.dmDisplayFrequency
        }
    }
    return $out
}

function Set-DisplayPosition([string]$name, [int]$x, [int]$y) {
    $dm = New-Object DEVMODE
    $dm.dmSize = [uint16][Runtime.InteropServices.Marshal]::SizeOf($dm)
    if (-not [Disp]::EnumDisplaySettings($name, [Disp]::ENUM_CURRENT_SETTINGS, [ref]$dm)) {
        throw "EnumDisplaySettings failed for $name"
    }
    $dm.dmPositionX = $x
    $dm.dmPositionY = $y
    $dm.dmFields    = [Disp]::DM_POSITION

    # Stage the change in the registry without resetting the mode, then commit
    # every staged change in one call. Doing it in two steps is what lets the
    # arrangement be validated as a whole rather than per-monitor, which matters
    # because an intermediate state can be non-contiguous and get rejected.
    $rc = [Disp]::ChangeDisplaySettingsEx($name, [ref]$dm, [IntPtr]::Zero,
                                          ([Disp]::CDS_UPDATEREGISTRY -bor [Disp]::CDS_NORESET),
                                          [IntPtr]::Zero)
    if ($rc -ne 0) { throw "ChangeDisplaySettingsEx staged call returned $rc for $name" }

    $rc = [Disp]::ApplyPending($null, [IntPtr]::Zero, [IntPtr]::Zero, 0, [IntPtr]::Zero)
    if ($rc -ne 0) { throw "apply returned $rc (0 = success, -2 = BADMODE, 1 = restart required)" }
}

function Show-Displays($d) {
    $d | ForEach-Object {
        $tag = ''
        if ($_.Primary) { $tag = '  [PRIMARY]' }
        "  {0,-16} {1,5},{2,-6} {3}x{4} @{5}Hz  {6}{7}" -f `
            $_.Name, $_.X, $_.Y, $_.Width, $_.Height, $_.Hz, $_.Model, $tag
    }
}

# --- revert ----------------------------------------------------------------

if ($Revert) {
    if (-not (Test-Path $BackupPath)) { throw "no backup at $BackupPath" }
    $saved = Get-Content $BackupPath -Raw | ConvertFrom-Json
    Write-Host "restoring saved arrangement from $BackupPath" -ForegroundColor Cyan
    foreach ($s in $saved) {
        Write-Host ("  {0} -> {1},{2}" -f $s.Name, $s.X, $s.Y)
        Set-DisplayPosition $s.Name ([int]$s.X) ([int]$s.Y)
    }
    Write-Host "done." -ForegroundColor Green
    Show-Displays (Get-Displays)
    return
}

# --- list ------------------------------------------------------------------

$displays = Get-Displays

if ($List -or $PSCmdlet.ParameterSetName -eq 'List') {
    Write-Host "attached displays:" -ForegroundColor Cyan
    Show-Displays $displays
    Write-Host ""
    Write-Host "Pick the one carrying the seat panel (mirror B \\.\DISPLAYn), then:"
    Write-Host "  .\hydra-display.ps1 -Device '\\.\DISPLAY2' -Above"
    return
}

# --- move ------------------------------------------------------------------

$target  = $displays | Where-Object Name -eq $Device
$primary = $displays | Where-Object Primary

if (-not $target)  { throw "no attached display named $Device. Run -List." }
if (-not $primary) { throw "no primary display found." }
if ($target.Name -eq $primary.Name) { throw "refusing to move the primary display." }
if (-not $Above)   { throw "only -Above is implemented. Left trades a quiet right edge for a busy left one." }

# Sit the target directly above the primary, overlapping horizontally by
# exactly $OverlapPx at the chosen corner. Everything outside that strip has
# no adjacent pixels, so the pointer cannot cross there.
$newY = $primary.Y - $target.Height
if ($Corner -eq 'Right') {
    $newX = $primary.X + $primary.Width - $OverlapPx
} else {
    $newX = $primary.X - $target.Width + $OverlapPx
}

Write-Host "current:" -ForegroundColor Cyan
Show-Displays $displays
Write-Host ""
Write-Host "plan:" -ForegroundColor Cyan
Write-Host ("  {0}  {1},{2}  ->  {3},{4}" -f $target.Name, $target.X, $target.Y, $newX, $newY)
Write-Host ("  contact strip: {0}px wide at the {1} end of the primary's top edge" -f $OverlapPx, $Corner.ToLower())
Write-Host ("  that strip spans x={0}..{1}, y={2}" -f `
    ([math]::Max($newX, $primary.X)), ([math]::Min($newX + $target.Width, $primary.X + $primary.Width)), $primary.Y)

if (-not $Apply) {
    Write-Host ""
    Write-Host "nothing changed. re-run with -Apply to commit." -ForegroundColor Yellow
    return
}

New-Item -ItemType Directory -Force -Path (Split-Path $BackupPath) | Out-Null
$displays | Select-Object Name, X, Y | ConvertTo-Json | Set-Content $BackupPath -Encoding UTF8
Write-Host ""
Write-Host "backup written to $BackupPath  (undo with -Revert)" -ForegroundColor DarkGray

Set-DisplayPosition $target.Name $newX $newY

Write-Host "applied." -ForegroundColor Green
Write-Host ""
Show-Displays (Get-Displays)
Write-Host ""
Write-Host "mirror targets the DEVICE (\\.\DISPLAY2), not coordinates, so the panel" -ForegroundColor DarkGray
Write-Host "should follow. If it lands wrong, restart that one mirror via hydrad." -ForegroundColor DarkGray
Write-Host "If anything is unreachable:  .\hydra-display.ps1 -Revert" -ForegroundColor DarkGray
