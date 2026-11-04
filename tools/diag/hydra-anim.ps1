# hydra-anim.ps1 -- the console account's animation switches, one at a time.
#
# No window can keep the virtual-desktop slide off seat B's panel, topmost or
# UIAccess (hydra-seat-hold*.ps1), so the plan is to switch the slide off while
# the seat is up. Settings > Accessibility > Visual effects > "Animation effects"
# is seven switches in one -- Performance Options lists them separately. This
# finds the fewest that stop the slide, so the launcher takes only those during
# a lesson and gives them back at -Stop.
#
# Per user, so seat B's account keeps its own. Takes effect at once, no sign-out.
# HKCU: run it as yourself (the elevated Hydra Shell is the same account).
#
# USAGE
#   .\hydra-anim.ps1                           # show the seven
#   .\hydra-anim.ps1 -Off Controls             # then slide and watch the panel
#   .\hydra-anim.ps1 -On Controls -Off Windows
#   .\hydra-anim.ps1 -Off All                  # = Settings' Animation effects: Off
#   .\hydra-anim.ps1 -On All

param(
    [ValidateSet('Controls', 'Windows', 'Menus', 'MenuItems', 'ToolTips', 'ComboBoxes', 'ListBoxes', 'All')]
    [string]$Off,
    [ValidateSet('Controls', 'Windows', 'Menus', 'MenuItems', 'ToolTips', 'ComboBoxes', 'ListBoxes', 'All')]
    [string]$On
)

if (-not ('HydraAnim' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class HydraAnim {
    [StructLayout(LayoutKind.Sequential)] struct ANIMATIONINFO { public uint cbSize; public int iMinAnimate; }
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, ref int p, uint f);
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, IntPtr p, uint f);
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, ref ANIMATIONINFO p, uint f);
    const uint SPI_GETANIMATION = 0x48, SPI_SETANIMATION = 0x49;
    const uint SPIF = 3;   // UPDATEINIFILE | SENDCHANGE: saved to the profile, and running apps are told

    // 'get' is the SPI_GET* code; every SPI_SET* here is get + 1.
    public static bool Get(uint get) {
        if (get == SPI_GETANIMATION) {   // min/max animation lives in a struct, not a BOOL
            ANIMATIONINFO ai = new ANIMATIONINFO();
            ai.cbSize = 8;
            SystemParametersInfo(SPI_GETANIMATION, 8, ref ai, 0);
            return ai.iMinAnimate != 0;
        }
        int v = 0;
        SystemParametersInfo(get, 0, ref v, 0);
        return v != 0;
    }

    public static bool Set(uint get, bool on) {
        if (get == SPI_GETANIMATION) {
            ANIMATIONINFO ai = new ANIMATIONINFO();
            ai.cbSize = 8;
            ai.iMinAnimate = on ? 1 : 0;
            return SystemParametersInfo(SPI_SETANIMATION, 8, ref ai, SPIF);
        }
        return SystemParametersInfo(get + 1, 0, new IntPtr(on ? 1 : 0), SPIF);   // the BOOL rides in pvParam
    }
}
'@
}

# Name = SPI_GET* code, and the label Performance Options (SystemPropertiesPerformance.exe) shows.
$knobs = [ordered]@{
    Controls   = @(0x1042, 'Animate controls and elements inside windows')
    Windows    = @(0x0048, 'Animate windows when minimizing and maximizing')
    Menus      = @(0x1002, 'Fade or slide menus into view')
    MenuItems  = @(0x1014, 'Fade out menu items after clicking')
    ToolTips   = @(0x1016, 'Fade or slide ToolTips into view')
    ComboBoxes = @(0x1004, 'Slide open combo boxes')
    ListBoxes  = @(0x1006, 'Smooth-scroll list boxes')
}
function Pick($name) { if ($name -eq 'All') { @($knobs.Keys) } else { @($name) } }

foreach ($k in $(if ($Off) { Pick $Off })) {
    if (-not [HydraAnim]::Set([uint32]$knobs[$k][0], $false)) { Write-Host "  could not switch $k off" -ForegroundColor Red }
}
foreach ($k in $(if ($On) { Pick $On })) {
    if (-not [HydraAnim]::Set([uint32]$knobs[$k][0], $true)) { Write-Host "  could not switch $k on" -ForegroundColor Red }
}

foreach ($k in $knobs.Keys) {
    $isOn = [HydraAnim]::Get([uint32]$knobs[$k][0])
    Write-Host ('{0,-4} {1,-11} {2}' -f $(if ($isOn) { 'on' } else { 'OFF' }), $k, $knobs[$k][1]) `
        -ForegroundColor $(if ($isOn) { 'DarkGray' } else { 'Yellow' })
}
