# hydra-shield-check.ps1 -- what the launcher's panel shielding looks like right
# now. Changes nothing; run it with the seat up, from the Hydra Shell.
#
# Reports: which launcher is installed, whether its helpers are where it looks,
# the two animation switches and the launcher's flags for them, whether the
# .rdp signing certificate exists, whether mstsc is topmost, and which z-order
# layer the cursor overlay and the seat hold are in (1 = ordinary, 2 = UIAccess).
# VIDEO-H264.md, 'Keeping the console's shell off the panel', has the why.
#
# USAGE
#   .\tools\diag\hydra-shield-check.ps1

param(
    [string]$Root   = 'C:\Programs\hydra',
    [string]$PsExec = 'C:\Programs\PSTools\PsExec64.exe'
)

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Mark($ok) { if ($ok) { 'Green' } else { 'Yellow' } }

if (-not ('HydraShieldCheck2' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class HydraShieldCheck2 {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] static extern bool GetWindowBand(IntPtr h, out uint band);
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, ref int p, uint f);
    [StructLayout(LayoutKind.Sequential)] struct ANIMATIONINFO { public uint cbSize; public int iMinAnimate; }
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, ref ANIMATIONINFO p, uint f);

    public static bool Topmost(IntPtr h) { return (GetWindowLong(h, -20) & 0x8) != 0; }   // WS_EX_TOPMOST
    public static string Band(IntPtr h) {
        uint b = 0;
        try { if (!GetWindowBand(h, out b)) return "?"; } catch (EntryPointNotFoundException) { return "n/a"; }
        return b.ToString();
    }
    public static bool ControlsAnimation() { int v = 0; SystemParametersInfo(0x1042, 0, ref v, 0); return v != 0; }
    public static bool WindowsAnimation() {
        ANIMATIONINFO ai = new ANIMATIONINFO();
        ai.cbSize = 8;
        SystemParametersInfo(0x48, 8, ref ai, 0);
        return ai.iMinAnimate != 0;
    }

    static string Cls(IntPtr h) { StringBuilder s = new StringBuilder(128); GetClassName(h, s, s.Capacity); return s.ToString(); }

    public static IntPtr Mstsc() {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            if (Cls(h) == "TscShellContainerClass" && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // One line per visible top-level window of the process: class, layer, topmost.
    public static string[] Windows(int pid) {
        List<string> rows = new List<string>();
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint p;
            GetWindowThreadProcessId(h, out p);
            if (p == (uint)pid && IsWindowVisible(h))
                rows.Add(Cls(h) + "  layer " + Band(h) + (Topmost(h) ? "  topmost" : ""));
            return true;
        }, IntPtr.Zero);
        return rows.ToArray();
    }
}
'@
}

Say 'files' Cyan
$launcher = Join-Path $Root 'hydra7-avc.ps1'
$isNew = (Test-Path $launcher) -and (Select-String -Path $launcher -Pattern 'HydraAvcShield' -Quiet)
Say ("  launcher        {0}" -f $(if ($isNew) { 'new (has the shielding)' } else { 'OLD -- no shielding in it' })) (Mark $isNew)
foreach ($f in (Join-Path $Root 'hydra-uia.ps1'), (Join-Path $Root 'hydra-seat-hold.ps1'),
               (Join-Path $Root 'hydra-rdp-trust.ps1'), $PsExec) {
    $ok = Test-Path $f
    Say ("  {0,-15} {1}" -f (Split-Path -Leaf $f), $(if ($ok) { 'present' } else { "MISSING ($f)" })) (Mark $ok)
}

Say 'animation switches' Cyan
foreach ($a in @(
        @{ Kind = 'Controls'; On = [HydraShieldCheck2]::ControlsAnimation(); Flag = 'controls-animation-off-for-lesson'
           What = 'the desktop slide' },
        @{ Kind = 'Windows';  On = [HydraShieldCheck2]::WindowsAnimation();  Flag = 'window-animation-off-for-lesson'
           What = "Task View's zoom" })) {
    $flag = Test-Path (Join-Path $Root "state\$($a.Flag)")
    Say ("  {0,-15} {1}" -f $a.Kind, $(if ($a.On) { "ON -- $($a.What) reaches the panel" } else { 'off' })) (Mark (-not $a.On))
    Say ("  {0,-15} {1}" -f '  launcher flag', $(if ($flag) { "set: it turns $($a.Kind) back on at -Stop" } else { 'not set' })) DarkGray
}

Say 'rdp signing' Cyan
$signer = Get-ChildItem Cert:\LocalMachine\My -EA SilentlyContinue |
    Where-Object { $_.Subject -eq 'CN=Hydra rdp signing' -and $_.HasPrivateKey } | Select-Object -First 1
Say ("  certificate     {0}" -f $(if ($signer) { "present ($($signer.Thumbprint))" } else { 'none -- mstsc asks Connect/Cancel; run hydra-rdp-trust.ps1 once' })) (Mark $signer)

Say 'seat client' Cyan
$m = [HydraShieldCheck2]::Mstsc()
if ($m -eq [IntPtr]::Zero) {
    Say '  no mstsc session window' Yellow
} else {
    $top = [HydraShieldCheck2]::Topmost($m)
    Say ("  mstsc           {0}, layer {1}" -f $(if ($top) { 'topmost' } else { 'NOT topmost -- Show desktop reaches the panel' }),
        [HydraShieldCheck2]::Band($m)) (Mark $top)
}

Say 'cursor overlay' Cyan
$ovs = @(Get-Process cursor_overlay_seat, cursor_overlay -EA SilentlyContinue)
if (-not $ovs) { Say '  not running' Yellow }
foreach ($p in $ovs) {
    Say ("  {0} pid {1}, session {2}" -f $p.ProcessName, $p.Id, $p.SessionId) DarkGray
    foreach ($w in [HydraShieldCheck2]::Windows($p.Id)) { Say "    $w" $(if ($w -match 'layer 2') { 'Green' } else { 'Yellow' }) }
}

Say 'seat hold' Cyan
$hold = [HydraShieldCheck2]::FindWindow('HydraSeatHold', $null)
if ($hold -eq [IntPtr]::Zero) {
    Say '  not up -- Task View and the Start menu reach the panel' Yellow
} else {
    $b = [HydraShieldCheck2]::Band($hold)
    Say "  up, layer $b" (Mark ($b -eq '2'))
}
