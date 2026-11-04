# hydra7-avc.ps1 -- mode 7 with H.264: mstsc as the client, encoded on QuickSync.
#
# Derived from hydra7.ps1 (repo 1dd16bf). hydra7.ps1 stays as it is -- the
# RemoteFX fallback. Stop, preflight, taskbar, service, pin and verify are
# hydra7's; what changed is the client section and its audio pin.
#
# WHY. Measured 2026-10-08 with hydra-gpu-probe.ps1, same clip, 60 s windows:
#   RemoteFX via sdl-freerdp     ~131 CPU-sec/min   (TermService 50, client 81)
#   H.264 on QuickSync via mstsc ~26-30 CPU-sec/min, under a 60 fps cap
# Seat B was never composed in software -- its dwm draws on the Iris Plus. The
# cost was RemoteFX on the CPU at both ends. This FreeRDP build cannot decode
# H.264 (event 162: AVC available: 0); mstsc can, in hardware.
#
# REQUIRES (the H.264 preflight checks these):
#   HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services
#     AVC444ModePreferred        = 1
#     AVCHardwareEncodePreferred = 1
#     bEnumerateHWBeforeSW       ABSENT. It moves seat B onto the 1660 Ti, whose
#                                H.264 encoder (NVIDIA MFT) takes RdpIdd down
#                                about 14 s in: reason 17, DriverFrameworks 10121.
#   HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services\Client
#     fClientDisableUDP          = 1. TCP only. Over UDP, mstsc dropped the
#                                loopback connection every few minutes (client
#                                1026 Reason=2308, server 0x80070040), and
#                                agent:B lost the desktop on every drop.
#   HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations
#     DWMFRAMEINTERVAL           = 15   (60 fps; absent = RDP's 30 fps cap)
#
# VIDEO-H264.md has the runs, the recipe and the traps.
#
# mstsc ignored selectedmonitors on this machine, so the window is placed with
# the window APIs and made full-screen through its own window menu.
#
# The seat's cursor: an RDP client never learns where the seat's pointer is,
# and draws the pointer image as YOUR cursor -- so neither mstsc nor FreeRDP
# shows the student's. agent:B publishes it (HydraSeatCursor, hydra_ipc.h) and
# dist\cursor_overlay_seat.exe draws it over the seat's panel, x8, from this
# session -- where seat B's Start menu, Task View and virtual desktops can't
# hide it. -OverlayScale 0 skips it.
#
# UNCHANGED: input isolation. seat_router -> agent:B injects the wireless pair
# into the session whatever the client is.
#
# THE CONSOLE'S SHELL STAYS OFF THE PANEL (2026-11-03). The panel is one of the
# console's monitors, so shell effects played there too. Now, while the seat is
# up:
#   desktop slide   'Animate controls and elements inside windows' and
#   Task View zoom  'Animate windows when minimizing and maximizing' are off
#                   for this account, back on at -Stop. No window can sit these
#                   out -- the slide covers even the UIAccess layer -- so the
#                   switches are the fix. -KeepAnimations leaves them alone.
#   Show desktop    the client is topmost, which Show desktop never covers.
#   Task View,      the cursor overlay and the seat hold (hydra-seat-hold.ps1, a
#   Start menu      click-through live DWM copy of the client) start with
#                   UIAccess, the on-screen keyboard's layer, above every shell
#                   surface. hydra-uia.ps1 sets that token flag as SYSTEM, via
#                   PsExec. -NoHold, or a missing piece, falls back to the
#                   ordinary overlay, and Task View reaches the panel again.
# Needs hydra-uia.ps1 and hydra-seat-hold.ps1 in -Root, and PsExec (-PsExec).
#
# NO CONNECT PROMPT. Since the April 2026 security update mstsc shows a
# Connect/Cancel dialog, Cancel first, whenever it opens an unsigned .rdp file.
# The fresh .rdp is signed on every launch with a certificate that
# hydra-rdp-trust.ps1 makes, once, a trusted .rdp publisher on this machine.
# Without that certificate the launcher still runs; it just asks.
#
# USAGE (elevated, from the Hydra Shell)
#   .\hydra7-avc.ps1
#   .\hydra7-avc.ps1 -Monitor 2        # skip auto-detect, use this FreeRDP index
#   .\hydra7-avc.ps1 -NoHold           # no UIAccess overlay or hold
#   .\hydra7-avc.ps1 -KeepAnimations   # leave the animation switch alone
#   .\hydra7-avc.ps1 -Stop

param(
    [int]$Monitor = 0,          # 0 = auto-detect the seat panel
    [switch]$Stop,
    [string]$Seat = 'B',
    [string]$User = 'teacher',
    [switch]$NoAudioPin,
    [int]$Scale = 150,          # session DPI %; mstsc takes 100/125/150/175/200
    [int]$OverlayScale = 8,     # cursor_overlay.exe size multiplier; 0 = don't start it
    [string]$Overlay = '',      # default: <Root>\dist\cursor_overlay_seat.exe
    [switch]$NoHold,            # no UIAccess overlay or seat hold: Task View reaches the panel
    [switch]$KeepAnimations,    # leave 'Animate controls...' on: the desktop slide reaches the panel
    [string]$PsExec = 'C:\Programs\PSTools\PsExec64.exe',
    [string]$Root = 'C:\Programs\hydra'
)

$ErrorActionPreference = 'Continue'
# PS 7.4 made native-command stderr honour ErrorActionPreference. Several tools
# here write PROGRESS to stderr -- hydractl's 'not reachable' while it waits,
# mirror's 'pixel transport opened' -- and 2>&1 under 'Stop' turned those
# SUCCESS lines into terminating errors. This broke hydra-start.ps1 on 2026-08-21.
$PSNativeCommandUseErrorActionPreference = $false
Set-Location $Root
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }
$me   = (Get-Process -Id $PID).SessionId
$self = Split-Path -Leaf $PSCommandPath

# ------------------------------------------- console taskbar on seat panel --
# Windows only hides a taskbar behind a fullscreen window while that window is
# in the foreground. In mode 7 the teacher works on the laptop, so the console's
# secondary taskbar sat on top of the seat's screen, above seat B's own taskbar.
# Off while the seat is up, back on at -Stop.
# HKCU: run elevated from the SAME account, or this edits the wrong hive.
$mmKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
# PID of the explorer that owns THIS session's taskbar. FindWindow only sees
# the calling session, so seat B's shell can never match.
# Removes a window's taskbar button (ITaskbarList::DeleteTab). A fresh COM
# object per call, so it always reaches the explorer running NOW -- no stale
# binding like the VirtualDesktop module has.
if (-not ('HydraTaskbar' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("56FDF342-FD6D-11d0-958A-006097C9A090"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ITaskbarList {
    void HrInit();
    void AddTab(IntPtr hwnd);
    void DeleteTab(IntPtr hwnd);
    void ActivateTab(IntPtr hwnd);
    void SetActiveAlt(IntPtr hwnd);
}
[ComImport, Guid("56FDF344-FD6D-11d0-958A-006097C9A090")]
class CTaskbarList { }
public static class HydraTaskbar {
    public static void Hide(IntPtr hwnd) {
        ITaskbarList t = (ITaskbarList)new CTaskbarList();
        try { t.HrInit(); t.DeleteTab(hwnd); }
        finally { Marshal.ReleaseComObject(t); }
    }
}
'@
}
if (-not ('HydraShellWin' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HydraShellWin {
    [DllImport("user32.dll")] static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public static int TrayPid() {
        IntPtr h = FindWindow("Shell_TrayWnd", null);
        if (h == IntPtr.Zero) return 0;
        uint pid; GetWindowThreadProcessId(h, out pid);
        return (int)pid;
    }
}
'@
}
if (-not ('HydraMstsc' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class HydraMstsc {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after,
        int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern IntPtr GetSystemMenu(IntPtr h, bool revert);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr m);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr m, int pos);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetMenuString(IntPtr m, uint item, StringBuilder s, int n, uint flags);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    // Per-monitor v2: physical pixels, the same coordinates FreeRDP and mstsc /l report.
    static void Aware() { SetThreadDpiAwarenessContext(new IntPtr(-4)); }
    public static IntPtr Find() {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            StringBuilder s = new StringBuilder(64);
            GetClassName(h, s, s.Capacity);
            if (s.ToString() == "TscShellContainerClass" && IsWindowVisible(h)) {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static int[] Rect(IntPtr h) {
        Aware();
        RECT r;
        GetWindowRect(h, out r);
        return new int[] { r.L, r.T, r.R, r.B };
    }
    public static void Move(IntPtr h, int x, int y, int w, int ht) {
        Aware();
        ShowWindow(h, 9);                                   // SW_RESTORE
        SetWindowPos(h, IntPtr.Zero, x, y, w, ht, 0x0014);  // NOZORDER | NOACTIVATE
    }
    // Returns "ok", or the menu's items so a miss can be diagnosed.
    public static string FullScreen(IntPtr h) {
        IntPtr m = GetSystemMenu(h, false);
        int n = GetMenuItemCount(m);
        StringBuilder all = new StringBuilder();
        for (int i = 0; i < n; i++) {
            StringBuilder s = new StringBuilder(128);
            GetMenuString(m, (uint)i, s, s.Capacity, 0x400);  // MF_BYPOSITION
            string t = s.ToString().Replace("&", "");
            all.Append("[" + t + "] ");
            if (t.IndexOf("full screen", StringComparison.OrdinalIgnoreCase) >= 0) {
                PostMessage(h, 0x0112, new IntPtr(GetMenuItemID(m, i)), IntPtr.Zero);  // WM_SYSCOMMAND
                return "ok";
            }
        }
        return all.ToString();
    }
}
'@
}
if (-not ('HydraAvcShield2' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HydraAvcShield2 {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after,
        int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool GetWindowBand(IntPtr h, out uint band);   // undocumented, Win8+
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, ref int p, uint f);
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, IntPtr p, uint f);
    [StructLayout(LayoutKind.Sequential)] struct ANIMATIONINFO { public uint cbSize; public int iMinAnimate; }
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint a, uint u, ref ANIMATIONINFO p, uint f);

    // HWND_TOPMOST; NOSIZE | NOMOVE | NOACTIVATE.
    public static bool Topmost(IntPtr h) { return SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x13); }

    public static IntPtr FindHold() { return FindWindow("HydraSeatHold", null); }
    public static void CloseHold(IntPtr h) { PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); }   // WM_CLOSE

    // z-order band: 1 = the ordinary one, 2 = UIAccess, 0 = could not read it.
    public static uint Band(IntPtr h) {
        uint b = 0;
        try { GetWindowBand(h, out b); } catch (EntryPointNotFoundException) { }
        return b;
    }
    // 2 if any visible window of the process is in the UIAccess band, else the
    // first band seen, else 0.
    public static uint ProcessBand(int pid) {
        uint seen = 0;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint p;
            GetWindowThreadProcessId(h, out p);
            if (p == (uint)pid && IsWindowVisible(h)) {
                uint b = Band(h);
                if (b == 2 || seen == 0) seen = b;
            }
            return true;
        }, IntPtr.Zero);
        return seen;
    }

    // The two animation switches a lesson takes (hydra-anim.ps1 names):
    //   Controls  'Animate controls and elements inside windows', SPI_GET/SETCLIENTAREAANIMATION:
    //             the desktop slide follows it
    //   Windows   'Animate windows when minimizing and maximizing', SPI_GET/SETANIMATION:
    //             Task View's zoom follows it
    public static bool Animation(string kind) {
        if (kind == "Windows") {
            ANIMATIONINFO ai = new ANIMATIONINFO();
            ai.cbSize = 8;
            SystemParametersInfo(0x48, 8, ref ai, 0);
            return ai.iMinAnimate != 0;
        }
        int v = 0;
        SystemParametersInfo(0x1042, 0, ref v, 0);
        return v != 0;
    }
    public static bool SetAnimation(string kind, bool on) {
        if (kind == "Windows") {
            ANIMATIONINFO ai = new ANIMATIONINFO();
            ai.cbSize = 8;
            ai.iMinAnimate = on ? 1 : 0;
            return SystemParametersInfo(0x49, 8, ref ai, 3);
        }
        return SystemParametersInfo(0x1043, 0, new IntPtr(on ? 1 : 0), 3);   // UPDATEINIFILE | SENDCHANGE
    }
}
'@
}
function Set-ConsoleTaskbarOnAllDisplays([bool]$On) {
    $want = [int]$On
    $have = (Get-ItemProperty $mmKey -Name MMTaskbarEnabled -EA SilentlyContinue).MMTaskbarEnabled
    if ($null -eq $have) { $have = 1 }      # absent = Windows default = on
    if ($have -eq $want) { return }         # no change, no explorer restart
    Set-ItemProperty `
        -Path $mmKey `
        -Name 'MMTaskbarEnabled' `
        -Value $want `
        -Type DWord
    # Folder windows in their own process, so killing the shell spares them.
    # Explorer reads this at start: the FIRST restart after it is set still
    # takes open folder windows with it. Every restart after that leaves them.
    if ((Get-ItemProperty $mmKey -Name SeparateProcess -EA SilentlyContinue).SeparateProcess -ne 1) {
        Set-ItemProperty `
            -Path $mmKey `
            -Name 'SeparateProcess' `
            -Value 1 `
            -Type DWord
        Say "  folder windows set to a separate process (open ones close this once)" Yellow
    }
    # Kill ONLY the shell: the explorer that owns this session's taskbar.
    # Not seat B's shell (other session), not the folder-window process.
    $old = [HydraShellWin]::TrayPid()
    if ($old) { Stop-Process -Id $old -Force }
    # Wait for a NEW taskbar, then let it settle. Virtual desktops live in the
    # shell; a window created while it is still starting can lose its pin.
    # (A surviving folder-window explorer means "any explorer running" is no
    # longer proof the shell is back -- hence the tray check.)
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep 1
        $now = [HydraShellWin]::TrayPid()
        if ($now -and $now -ne $old) { break }
    }
    if (-not [HydraShellWin]::TrayPid()) { Start-Process explorer }
    Start-Sleep 4
    Say "  console taskbar on all displays: $(if ($On) { 'on' } else { 'off' })" DarkGray
}

# ------------------------------------------------- lesson animations -------
# The desktop slide (Ctrl+Win+arrows, four-finger swipe, Task View) plays on
# every monitor of this session, and no window can sit it out: it covered even
# a UIAccess window (hydra-seat-hold-uia.ps1). Task View's zoom reaches the
# panel the same way. Each follows one of the switches hydra-anim.ps1 found
# (-Off Controls, -Off Windows), so both are off while the seat is up -- and
# each comes back at -Stop only if this script turned it off: a flag per switch
# in state\. Per user, so seat B's account keeps its own. HKCU, so the
# same-account rule of the taskbar toggle applies.
$lessonAnimations = @(
    @{ Kind = 'Controls'; Flag = 'controls-animation-off-for-lesson'
       Name = 'Animate controls and elements inside windows' }       # the desktop slide
    @{ Kind = 'Windows';  Flag = 'window-animation-off-for-lesson'
       Name = 'Animate windows when minimizing and maximizing' }     # Task View's zoom
)
function Set-LessonAnimations([bool]$Lesson) {
    foreach ($a in $lessonAnimations) {
        $flag = Join-Path $Root "state\$($a.Flag)"
        if ($Lesson) {
            if (-not [HydraAvcShield2]::Animation($a.Kind)) { continue }    # already off: not ours to give back
            if ([HydraAvcShield2]::SetAnimation($a.Kind, $false)) {
                New-Item -ItemType Directory -Force -Path (Split-Path $flag) | Out-Null
                New-Item -ItemType File -Force -Path $flag | Out-Null
                Say "  '$($a.Name)' off for the lesson" DarkGray
            } else {
                Say "  could not switch '$($a.Name)' off -- its animation will reach the panel" Yellow
            }
        } elseif (Test-Path $flag) {
            [void][HydraAvcShield2]::SetAnimation($a.Kind, $true)
            Remove-Item $flag -Force
            Say "  '$($a.Name)' back on" DarkGray
        }
    }
}

# ------------------------------------------------------------ UIAccess -----
# Task View and the Start menu draw above every ordinary window on every
# monitor, topmost included. UIAccess windows sit above them -- the on-screen
# keyboard's layer. Windows grants UIAccess to signed exes under Program Files;
# what it checks is a flag on the process token, which SYSTEM may set.
# hydra-uia.ps1 does that, run as SYSTEM in session 0 through PsExec, and starts
# the command in THIS session, as you, its console minimized. Returns $true if it
# started.
$ps51      = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$uiaScript = Join-Path $Root 'hydra-uia.ps1'
function Start-UiAccess([string]$CommandLine, [bool]$Elevated) {
    $spec = @{ session = $me; cmd = $CommandLine; dir = $Root; elevated = $Elevated } |
        ConvertTo-Json -Compress
    $job = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($spec))
    # -InputFormat None and an empty stdin: under PsExec, powershell.exe
    # otherwise sits waiting for input that never comes.
    $out = $null | & $PsExec -accepteula -nobanner -s `
        $ps51 -NoProfile -NonInteractive -InputFormat None -ExecutionPolicy Bypass `
        -File $uiaScript -Job $job 2>&1 | Out-String
    # PsExec hands back hydra-uia.ps1's exit code, but not reliably its output:
    # the first run of this saw exit 0, a started overlay, and no "started" line.
    # So the exit code is the verdict.
    if ($LASTEXITCODE -eq 0) { return $true }
    Say "  UIAccess start failed (PsExec exit $LASTEXITCODE):" Yellow
    foreach ($l in ($out -split "`r?`n")) { if ($l.Trim()) { Say "    $($l.Trim())" DarkGray } }
    return $false
}

# ---------------------------------------------------------------- stop -----
function Stop-Everything {
    Say "stopping ..." Cyan
    # The seat hold first, so it is not left over the panel showing a dead client.
    $hold = [HydraAvcShield2]::FindHold()
    if ($hold -ne [IntPtr]::Zero) { [HydraAvcShield2]::CloseHold($hold) }
    Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay, cursor_overlay_seat `
        -EA SilentlyContinue | Stop-Process -Force
    # Each overlay's PowerShell host outlives it (-NoExit, so a failure stays
    # readable). Close them too, or every launch leaves one more behind.
    Get-Process powershell -EA SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like 'Hydra seat * -- cursor overlay' } |
        Stop-Process -Force -EA SilentlyContinue
    Stop-Service Hydra -EA SilentlyContinue
    Start-Sleep 2
    $t = query session | Select-String $User
    if ($t) {
        $sid = ($t.ToString().Trim() -split '\s+' | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1)
        # A merely DISCONNECTED session is held by RDP-Wrapper and only a reboot
        # clears it. Log it off properly.
        if ($sid) { Say "  logging off $User session $sid" Yellow; logoff $sid 2>$null }
    }
    Say "stopped." Green
}

if ($Stop) { Stop-Everything; Set-LessonAnimations $false; Set-ConsoleTaskbarOnAllDisplays $true; return }
Stop-Everything
Say ""

# ---------------------------------------------------------- preconditions --
if ((& sc.exe qc TermService | Out-String) -notmatch 'WIN32_OWN_PROCESS') {
    Say "TermService is not type= own. RDP-Wrapper's ServiceDll will not load," Red
    Say "you get ONE session, and it shows up as ERRCONNECT_ACTIVATION_TIMEOUT." Red
    Say "  sc.exe config TermService type= own      (then reboot)" Red
    return
}

$dm = (Select-String -Path "$Root\dist\seats.toml" -Pattern '^display_mode' | Select-Object -First 1).Line
if ($dm -notmatch '"off"') {
    Say "display_mode is not `"off`" -- currently: $dm" Yellow
    Say "Mode 7 needs it off, or hydrad launches a capture agent that nothing reads." Yellow
    Say "  (Get-Content seats.toml -Raw) -replace '(?m)^display_mode = `".*`"', 'display_mode = `"off`"' | Set-Content seats.toml -NoNewline" Yellow
    Say "  .\setup.ps1" Yellow
    return
}

# ------------------------------------------------------------- service -----
# Still needed: it runs seat_router and agent:B, which are what actually give
# the student their keyboard and mouse.
# --- termsrv preflight (see tools\termsrv-guard) -------------------------
# Fail fast with the real cause instead of a seat that times out or is
# logged off ten seconds in.
# $Root, not $PSScriptRoot: this copy may not live next to state\.
$tsFlag = Join-Path $Root 'state\termsrv-UNPATCHED'
if (Test-Path $tsFlag) {
    Write-Host "termsrv.dll is NOT patched (build $(Get-Content $tsFlag -Raw)) -- seat B would be logged off." -ForegroundColor Red
    Write-Host 'See tools\termsrv-guard\README.md, "Adding a pattern".' -ForegroundColor Red
    exit 1
}
$tsKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server'
if ((Get-ItemProperty $tsKey).fDenyTSConnections -eq 1) {
    Write-Host 'Remote Desktop is switched off (fDenyTSConnections=1) -- see INSTALL.md, termsrv-guard section.' -ForegroundColor Red
    exit 1
}
if ((Get-Service TermService).Status -ne 'Running') {
    Write-Host 'TermService not running -- starting it ...'
    Start-Service TermService
}
# --- end termsrv preflight -----------------------------------------------
# --- H.264 preflight -------------------------------------------------------
# bEnumerateHWBeforeSW is the dangerous one: it puts seat B on the 1660 Ti,
# and the NVIDIA H.264 encoder takes RdpIdd down about 14 seconds in.
$tsPol = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services'
$pol   = Get-ItemProperty $tsPol -EA SilentlyContinue
if ($pol.bEnumerateHWBeforeSW -eq 1) {
    Write-Host 'bEnumerateHWBeforeSW=1 puts seat B on the 1660 Ti, whose H.264 encoder crashes RdpIdd.' -ForegroundColor Red
    Write-Host "  Remove-ItemProperty -Path '$tsPol' -Name bEnumerateHWBeforeSW    (then reboot)" -ForegroundColor Red
    exit 1
}
if ($pol.AVC444ModePreferred -ne 1 -or $pol.AVCHardwareEncodePreferred -ne 1) {
    Say 'H.264 is not fully enabled (AVC444ModePreferred, AVCHardwareEncodePreferred):' Yellow
    Say '  seat B will be encoded on the CPU. See this script''s header.' Yellow
}
$wsKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations'
if ((Get-ItemProperty $wsKey -EA SilentlyContinue).DWMFRAMEINTERVAL -ne 15) {
    Say 'DWMFRAMEINTERVAL is not 15 -- seat B is capped at 30 fps.' DarkGray
}
$cli = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services\Client'
if ((Get-ItemProperty $cli -EA SilentlyContinue).fClientDisableUDP -ne 1) {
    Say 'fClientDisableUDP is not 1 -- mstsc will use UDP and seat B will drop.' Yellow
    Say '  .\hydra-gpu-probe.ps1 -Apply sets the whole recipe (VIDEO-H264.md).' Yellow
}
# --- end H.264 preflight ---------------------------------------------------
# Console taskbar off the seat panel. After the preflight, so a failed
# preflight does not leave it switched off. Before the service so explorer has the
# service start and audio pin to finish restarting before the client window
# exists -- a window born into a half-started explorer can lose its pin.
Set-ConsoleTaskbarOnAllDisplays $false
if (-not $KeepAnimations) { Set-LessonAnimations $true }

Say "starting Hydra service ..." Cyan
Start-Service Hydra
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep 1
    if ((& "$Root\dist\hydractl.exe" status 2>&1 | Out-String) -notmatch 'not reachable') { break }
}

# ----------------------------------------------------------- audio pin -----
# MUST run BEFORE the client starts -- a per-app output change will not take on
# an audio stream that is already open (audio-pin.ps1 line 23).
#
# mstsc plays the seat's audio into the console session, as sdl-freerdp did, so
# it needs its own pin -- in its OWN state file, so audio-pin.json (and with it
# hydra7.ps1) stays intact. abren sitting at "waiting for the shared ring" is
# still expected, not a fault.
$pinState = "$Root\audio-pin-mstsc.json"
if (-not $NoAudioPin -and (Test-Path "$Root\audio-pin.ps1")) {
    Say "applying audio pin ..." Cyan
    try { & "$Root\audio-pin.ps1" -Apply -App 'mstsc.exe' -StateFile $pinState | Out-Null }
    catch { Say "  audio-pin failed: $_" Yellow }
}

# ----------------------------------------------------------- seat panel ----
# FreeRDP's list is still the best source: it NAMES the monitors, mstsc /l only
# numbers them. The seat panel is the entry that is neither primary (marked *)
# nor the virtual display. -Monitor picks a FreeRDP index instead.
$mon = & "$Root\dist\freerdp\sdl-freerdp.exe" /list:monitor 2>&1 | Out-String
$panel = $null
foreach ($line in ($mon -split "`r?`n")) {
    if ($line -match '^\s*(\*)?\s*\[(\d+)\]\s+\[([^\]]+)\]\s+(\d+)x(\d+)\s+\+(-?\d+)\+(-?\d+)') {
        $m = [pscustomobject]@{
            Primary = [bool]$Matches[1]
            Index   = [int]$Matches[2]
            Name    = $Matches[3]
            W       = [int]$Matches[4]
            H       = [int]$Matches[5]
            X       = [int]$Matches[6]
            Y       = [int]$Matches[7]
        }
        if ($Monitor -gt 0) {
            if ($m.Index -eq $Monitor) { $panel = $m }
        } elseif (-not $panel -and -not $m.Primary -and $m.Name -notmatch 'VDD') {
            $panel = $m
        }
    }
}
if (-not $panel) {
    Say "could not identify the seat panel. Pick one and pass -Monitor <n>:" Red
    Write-Host $mon
    return
}
Say ('seat panel: FreeRDP monitor {0}  [{1}]  {2}x{3} at {4},{5}' -f `
    $panel.Index, $panel.Name, $panel.W, $panel.H, $panel.X, $panel.Y) Green

# ------------------------------------------------------------- client ------
# A fresh .rdp every launch, so it always matches the panel as it is now.
#   screen mode id:i:2, use multimon:i:0, winposstr
#                             full screen on the panel, the old way of choosing a
#                             monitor. The move below does not rely on it.
#   displayconnectionbar:i:0  no connection bar on the student's screen: its
#                             minimize button would freeze the seat
#   keyboardhook:i:0          Windows-key combos stay on the console even when
#                             mstsc has focus. The student's keyboard reaches
#                             seat B through agent:B regardless.
#   redirect...:i:0           no clipboard, printers, drives or smart cards
#                             shared between your session and the student's
#   desktopscalefactor        session DPI. hydra7 used /scale:140; mstsc only
#                             takes 100/125/150/175/200... -- hence -Scale 150.
# Ticking "Remember me" in the password prompt stops it asking. Same exposure
# as rdsprov's stored password: anyone using this machine can use it.
$rdp = Join-Path $Root 'teacher-mstsc.rdp'
Set-Content -Path $rdp -Encoding Unicode -Value @(
    'full address:s:127.0.0.2'
    "username:s:$User"
    'screen mode id:i:2'
    'use multimon:i:0'
    "desktopwidth:i:$($panel.W)"
    "desktopheight:i:$($panel.H)"
    ('winposstr:s:0,1,{0},{1},{2},{3}' -f `
        $panel.X, $panel.Y, ($panel.X + $panel.W), ($panel.Y + $panel.H))
    "desktopscalefactor:i:$Scale"
    'session bpp:i:32'
    'audiomode:i:0'
    'authentication level:i:0'
    'connection type:i:6'
    'networkautodetect:i:0'
    'autoreconnection enabled:i:1'
    'displayconnectionbar:i:0'
    'pinconnectionbar:i:0'
    'keyboardhook:i:0'
    'redirectclipboard:i:0'
    'redirectprinters:i:0'
    'redirectcomports:i:0'
    'redirectsmartcards:i:0'
    'drivestoredirect:s:'
)
# Signed by the publisher hydra-rdp-trust.ps1 made trusted, mstsc opens it
# without the Connect/Cancel security dialog. rdpsign signs in place; nothing
# may touch the file after this, or the signature breaks.
$signer = Get-ChildItem Cert:\LocalMachine\My -EA SilentlyContinue |
    Where-Object { $_.Subject -eq 'CN=Hydra rdp signing' -and $_.HasPrivateKey } |
    Select-Object -First 1
if ($signer) {
    $rdpsign = Join-Path $env:WINDIR 'System32\rdpsign.exe'
    $certSha256 = -join ([Security.Cryptography.SHA256]::Create().ComputeHash($signer.RawData) |
        ForEach-Object { $_.ToString('X2') })
    # The verdict is the signature line in the file, not rdpsign's exit code.
    $signOut = & $rdpsign /sha256 $certSha256 $rdp 2>&1 | Out-String
    if (-not (Select-String -Path $rdp -Pattern '^signature:s:' -Quiet)) {
        # Some builds want the SHA1 thumbprint after /sha256.
        $signOut += & $rdpsign /sha256 $signer.Thumbprint $rdp 2>&1 | Out-String
    }
    if (Select-String -Path $rdp -Pattern '^signature:s:' -Quiet) {
        Say ".rdp signed: mstsc should connect without the Connect/Cancel prompt" Green
    } else {
        Say "  could not sign the .rdp, so mstsc will ask: $($signOut.Trim())" Yellow
    }
} else {
    Say "  .rdp unsigned, so mstsc asks Connect/Cancel -- hydra-rdp-trust.ps1, once, ends that" DarkGray
}
Say ""
Say "starting mstsc on the seat panel -- LOG IN AS $User in its prompt" Cyan
Start-Process mstsc -ArgumentList "`"$rdp`""

Say "waiting for the seat session ..." Yellow
$ok = $false
$sid = $null
for ($i = 0; $i -lt 90; $i++) {
    Start-Sleep 2
    if ((& query session | Out-String) -match "$User\s+(\d+)\s+Active") {
        $sid = [int]$Matches[1]
        $ok = $true
        break
    }
}
if (-not $ok) { Say "no $User session after 3 minutes." Red; return }
Say "seat session up." Green

# ---------------------------------------------------- full screen, here ----
# Window APIs, not keys: onto the panel, then "Full screen" from mstsc's own
# window menu (WM_SYSCOMMAND). Checked by the window covering exactly the panel.
function Test-OnPanel($r) {
    $r[0] -eq $panel.X -and $r[1] -eq $panel.Y -and
    $r[2] -eq ($panel.X + $panel.W) -and $r[3] -eq ($panel.Y + $panel.H)
}
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 15 -and $h -eq [IntPtr]::Zero; $i++) {
    Start-Sleep 1
    $h = [HydraMstsc]::Find()
}
if ($h -eq [IntPtr]::Zero) {
    Say "  no mstsc session window found -- the seat is up but not placed." Yellow
} elseif (Test-OnPanel ([HydraMstsc]::Rect($h))) {
    Say "mstsc is full-screen on the seat panel" Green
} else {
    Start-Sleep 2
    [HydraMstsc]::Move($h, ($panel.X + 40), ($panel.Y + 40), `
        [int]($panel.W * 0.75), [int]($panel.H * 0.75))
    Start-Sleep 1
    $res = [HydraMstsc]::FullScreen($h)
    Start-Sleep 3
    $r = [HydraMstsc]::Rect($h)
    if ($res -ne 'ok') {
        Say "  no 'Full screen' in mstsc's window menu. It has: $res" Yellow
    } elseif (Test-OnPanel $r) {
        Say "mstsc is full-screen on the seat panel" Green
    } else {
        Say ('  mstsc is not on the panel: window at {0},{1} - {2},{3}' -f $r[0], $r[1], $r[2], $r[3]) Yellow
    }
}

# -------------------------------------------------------------- pin --------
# A fullscreen client belongs to the virtual desktop it was launched from, and
# virtual desktops span all monitors -- so switching away hides the seat's
# screen from the student. Pinning puts it on every desktop.
#
# The pin runs in a FRESH PowerShell process every time. The VirtualDesktop
# module binds to explorer's COM objects once, when it first loads, and keeps
# them for the life of the process. After explorer restarts (the taskbar
# toggle, -Stop, a crash), a shell that already loaded the module is talking
# to a dead explorer and every pin silently fails. A child process always binds
# to the explorer that is running now.
Start-Sleep 3
$pinScript = {
    param([long]$hwnd)
    try {
        Import-Module -Name VirtualDesktop -DisableNameChecking -EA Stop
        Pin-Window -Hwnd ([IntPtr]$hwnd) | Out-Null
        if (Test-WindowPinned -Hwnd ([IntPtr]$hwnd)) { exit 0 }
        'pin call returned but Test-WindowPinned says no'
        exit 1
    } catch { "$_"; exit 2 }
}
$psExe = (Get-Process -Id $PID).Path
$pinned = $false
$pinMsg = 'no client window found'
for ($i = 1; $i -le 10 -and -not $pinned; $i++) {
    $h = [HydraMstsc]::Find()
    if ($h -ne [IntPtr]::Zero) {
        $pinMsg = & $psExe -NoProfile -Command $pinScript -args $h.ToInt64() 2>&1 | Out-String
        $pinned = ($LASTEXITCODE -eq 0)
    }
    if (-not $pinned) { Start-Sleep 2 }
}
if ($pinned) {
    Say "client pinned to all virtual desktops" Green
} else {
    Say "  client NOT pinned after 10 tries: $($pinMsg.Trim())" Yellow
    Say "  if the panel goes blank when you switch virtual desktops, that is why." Yellow
}

# ------------------------------------------------------------- topmost ------
# Show desktop (the taskbar's far corner, Win+D) minimizes what it can, then
# raises the desktop over every window that is not topmost -- on the panel,
# over the seat. Topmost, the client sits it out (hydra-topmost.ps1). It stays
# in the ordinary layer, below the overlay and the hold. With -NoHold, clicking
# into the seat can lift it over the ordinary overlay.
$h = [HydraMstsc]::Find()
if ($h -ne [IntPtr]::Zero -and [HydraAvcShield2]::Topmost($h)) {
    Say "client topmost: Show desktop leaves the panel alone" Green
} else {
    Say "  could not make the client topmost -- Show desktop will reach the panel." Yellow
}

# ------------------------------------------------------ taskbar button ------
# With the console taskbar off the seat panel, the client's button lands on the
# laptop's taskbar -- on every virtual desktop, since it is pinned. The teacher
# never needs it: the seat is reached by moving the cursor onto its panel.
$h = [HydraMstsc]::Find()
if ($h -ne [IntPtr]::Zero) {
    try   { [HydraTaskbar]::Hide($h); Say "client taskbar button removed" Green }
    catch { Say "  could not remove the client taskbar button: $_" Yellow }
}

# ------------------------------------------ cursor overlay and seat hold ----
# Seat mode: the overlay draws the cursor agent:B publishes, over the seat's
# panel, from THIS session (see the header). It runs in a minimized PowerShell
# like hydra7's client: a minimized window of its own would hand the overlay
# its first ShowWindow state and minimize the overlay too.
#
# Both the overlay and the hold start with UIAccess (Start-UiAccess above),
# above every shell surface: Task View and the Start menu stay on the laptop.
# The hold covers the panel with a live copy of the client and tucks itself
# directly under the overlay. The overlay goes first, with your full token as
# before; if it misses the UIAccess layer, there is no hold, because the hold
# would cover the student's cursor. -NoHold, or a missing piece, starts the
# ordinary overlay, as hydra7-avc always did.
if (-not $Overlay) { $Overlay = Join-Path $Root 'dist\cursor_overlay_seat.exe' }
$holdScript = Join-Path $Root 'hydra-seat-hold.ps1'
$wantOverlay = $OverlayScale -gt 0
if ($wantOverlay -and -not (Test-Path $Overlay)) {
    Say "cursor overlay not built ($Overlay) -- see VIDEO-H264.md, 'The seat's cursor'." Yellow
    $wantOverlay = $false
}
$shield = -not $NoHold
if ($shield) {
    $missing = @($PsExec, $uiaScript, $holdScript | Where-Object { -not (Test-Path $_) })
    if ($missing) {
        Say "no seat hold -- missing: $($missing -join ', ')" Yellow
        $shield = $false
    }
}
function Wait-Overlay {
    for ($i = 0; $i -lt 10; $i++) {
        $p = Get-Process cursor_overlay_seat -EA SilentlyContinue |
            Where-Object SessionId -eq $me | Select-Object -First 1
        if ($p) { return $p }
        Start-Sleep -Milliseconds 500
    }
    return $null
}
$at = '{0},{1},{2},{3}' -f $panel.X, $panel.Y, $panel.W, $panel.H
$ovCmd = "`$host.UI.RawUI.WindowTitle = 'Hydra seat $Seat -- cursor overlay'; " +
         "& '$Overlay' $OverlayScale --seat $Seat --at $at"
$ovUp = $false
if ($wantOverlay -and $shield) {
    $ovLine = "`"$ps51`" -NoProfile -NoExit -EncodedCommand " +
              [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($ovCmd))
    $ov = $null
    if (Start-UiAccess $ovLine $true) { $ov = Wait-Overlay }
    if ($ov) {
        $ovUp = $true
        Start-Sleep 1                                   # its window, not just its process
        $band = [HydraAvcShield2]::ProcessBand($ov.Id)
        if ($band -eq 2) {
            Say "cursor overlay drawing seat $Seat's cursor (x$OverlayScale, UIAccess)" Green
        } elseif ($band -eq 0) {
            Say "cursor overlay drawing seat $Seat's cursor (x$OverlayScale); its layer could not be read" Yellow
        } else {
            Say "cursor overlay up (x$OverlayScale) but not in the UIAccess layer -- no seat hold." Yellow
            $shield = $false
        }
    } else {
        Say "  no UIAccess overlay -- starting the ordinary one, and no seat hold." Yellow
        $shield = $false
    }
}
if ($wantOverlay -and -not $ovUp) {
    Start-Process powershell -WindowStyle Minimized -ArgumentList '-NoExit', '-Command', $ovCmd
    if (Wait-Overlay) {
        Say "cursor overlay drawing seat $Seat's cursor (x$OverlayScale)" Green
    } else {
        Say "cursor overlay did not start -- its minimized window says why." Yellow
    }
}
$held = $false
if ($shield) {
    $holdLine = "`"$ps51`" -NoProfile -ExecutionPolicy Bypass -File `"$holdScript`" -Run"
    if (Start-UiAccess $holdLine $false) {
        # Windows PowerShell compiles the hold's C# first: give it time.
        $hh = [IntPtr]::Zero
        for ($i = 0; $i -lt 60 -and $hh -eq [IntPtr]::Zero; $i++) {
            Start-Sleep -Milliseconds 250
            $hh = [HydraAvcShield2]::FindHold()
        }
        if ($hh -eq [IntPtr]::Zero) {
            Say "  seat hold did not come up -- its minimized window says why." Yellow
        } else {
            $held = $true
            $hb = [HydraAvcShield2]::Band($hh)
            Say "seat hold over the panel (layer $hb): Task View and the Start menu stay on the laptop" `
                $(if ($hb -eq 2) { 'Green' } else { 'Yellow' })
        }
    }
}
if (-not $held) { Say "  Task View and the Start menu will show on the panel." DarkGray }

# ------------------------------------------------------------- verify ------
Start-Sleep 2
Say ""
$st = & "$Root\dist\hydractl.exe" status 2>&1 | Out-String
Write-Host $st -ForegroundColor DarkGray
if ($st -match 'capture:B') {
    Say "capture:B is running -- display_mode is not off. Harmless but wasteful." Yellow
}
Say "expected in mode 7: router, agent:B, abcap:B, abren:B -- and NO capture:B" DarkGray

Say ""
Say "The student's wireless keyboard and mouse drive seat $Seat directly." Cyan
Say "No window, no focus, no virtual desktop involved." DarkGray

# Mouse button swap is PER USER, and seat B runs as a different account -- so a
# left-handed console does not make seat B left-handed. Crossing onto the seat's
# panel silently flips the buttons back, which is disorienting mid-lesson.
$mineSwapped = (Get-ItemProperty 'HKCU:\Control Panel\Mouse' SwapMouseButtons -EA SilentlyContinue).SwapMouseButtons
if ($mineSwapped -eq '1') {
    Say ""
    Say "NOTE: your console mouse is left-handed. Seat $Seat is a separate user," Yellow
    Say "so it has its own setting. If the buttons flip when you cross onto the" Yellow
    Say "seat's panel, set it once INSIDE the seat:" Yellow
    Say "  Settings > Bluetooth & devices > Mouse > Primary mouse button: Right" DarkGray
    Say "It persists in $User's profile after that." DarkGray
}
Say ""
Say "To reach seat $Seat yourself, move your cursor onto its panel." DarkGray
Say ""
Say "Stop:   .\$self -Stop" Cyan
Say "Panic:  type  Stop-Service Hydra  and press Enter (works blind)" DarkGray

# --------------------------------------------------------- audio note ------
$pin = Get-Content $pinState -Raw -EA SilentlyContinue
# Windows keeps one entry per change and never prunes them; -Save captures them
# all, and a stale one can win -- "looks right, plays nowhere" (audio-pin.ps1).
$pinRows = @()
if ($pin) { foreach ($r in ($pin | ConvertFrom-Json)) { $pinRows += $r } }
$stale = @($pinRows | Where-Object { $_.Value -notmatch 'intcdaud' })
if ($pin -and $pin -match 'intcdaud' -and $stale.Count) {
    Say ""
    Say "AUDIO PIN FOR mstsc HAS $($stale.Count) STALE ENTRY/ENTRIES besides the monitor." Yellow
    Say "A stale one can win, and seat B then plays nowhere. Keep only the monitor's:" Yellow
    Say "  `$k = @((Get-Content '$pinState' -Raw | ConvertFrom-Json) | Where-Object { `$_.Value -match 'intcdaud' })" DarkGray
    Say "  ConvertTo-Json -InputObject `$k | Set-Content '$pinState' -Encoding UTF8" DarkGray
}
if (-not $pin -or $pin -notmatch 'intcdaud') {
    Say ""
    Say "AUDIO PIN FOR mstsc IS NOT SET TO THE MONITOR." Yellow
    Say "Until it is, seat $Seat's sound follows the console's output device. Fix it ONCE:" Yellow
    Say "  1. play something in seat $Seat" DarkGray
    Say "  2. Settings > Sound > Volume mixer > mstsc.exe" DarkGray
    Say "     > Output device > 2770 (Intel Display Audio)" DarkGray
    Say "  3. .\audio-pin.ps1 -Save -App 'mstsc.exe' -StateFile '$pinState'" DarkGray
    Say "One entry naming intcdaud means it is right. -Apply then restores it" DarkGray
    Say "on every launch, before mstsc opens its stream." DarkGray
}
