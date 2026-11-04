# hydra-seat-hold.ps1 -- hold seat B's picture on its panel while the console
# switches virtual desktops.
#
# WHY. The desktop slide (Ctrl+Win+arrows, four-finger swipe, Task View) runs on
# every monitor of the console session, and the seat panel is one of them.
# Windows has no per-monitor setting for it. Pinning mstsc does not help: a
# window shown on all desktops sits the slide out and pops back afterwards.
# That pop is the blink the student sees.
#
# WHAT. A topmost, click-through, no-activate tool window over the panel,
# showing a live DWM thumbnail of mstsc's session window. A tool window is on
# no virtual desktop, so the shell has nothing to switch, and the thumbnail
# draws from mstsc's surface, which should keep updating while the slide hides
# the window itself. The laptop's screen keeps its slide.
#
# THE OPEN QUESTION is whether the slide hides topmost windows too. The taskbar
# rides it out. If the seat still blinks with this up, watch the student's
# cursor during the blink: if the overlay vanishes as well, Windows hides every
# window for the slide and this route is closed.
#
# Stacking: directly under the cursor overlay, so the student's cursor stays on
# top. Clicks pass through to mstsc, so reaching the seat by moving onto its
# panel works as before. A laptop window that strays onto the panel now sits
# UNDER the hold: the student never sees it, but your clicks land on it blind.
#
# Run it ELEVATED, like the launcher: mstsc and the cursor overlay run elevated,
# and UIPI can stop an unelevated process restacking them. It exits by itself
# 10 s after mstsc goes away, so hydra7-avc.ps1 -Stop takes it down too.
#
# USAGE (elevated, from the Hydra Shell, with the seat up)
#   .\hydra-seat-hold.ps1          # up, in its own minimized window
#   .\hydra-seat-hold.ps1 -Stop

param(
    [switch]$Stop,
    [switch]$Run                # internal: the child process that owns the window
)

$ErrorActionPreference = 'Continue'
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

if (-not ('HydraSeatHold' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class HydraSeatHold {
    public const string ClassName = "HydraSeatHold";

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct MSG {
        public IntPtr hwnd; public uint message; public IntPtr wParam, lParam;
        public uint time; public int x, y;
    }
    // dwmapi.h declares this inside pshpack1.h: no padding after 'opacity'.
    [StructLayout(LayoutKind.Sequential, Pack = 1)] struct THUMBPROPS {
        public uint flags; public RECT dest, src;
        public byte opacity; public int visible, clientOnly;
    }
    delegate IntPtr WndProcFn(IntPtr h, uint m, IntPtr w, IntPtr l);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct WNDCLASSEX {
        public uint cbSize, style; public WndProcFn lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        public string lpszMenuName, lpszClassName;
        public IntPtr hIconSm;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern ushort RegisterClassEx(ref WNDCLASSEX wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateWindowEx(uint ex, string cls, string title, uint style,
        int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetMessage(out MSG m, IntPtr h, uint lo, uint hi);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern void PostQuitMessage(int code);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr SetTimer(IntPtr h, IntPtr id, uint ms, IntPtr fn);
    [DllImport("user32.dll")] static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after,
        int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("gdi32.dll")] static extern IntPtr GetStockObject(int obj);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
    [DllImport("dwmapi.dll")] static extern int DwmRegisterThumbnail(IntPtr dest, IntPtr src, out IntPtr thumb);
    [DllImport("dwmapi.dll")] static extern int DwmUnregisterThumbnail(IntPtr thumb);
    [DllImport("dwmapi.dll")] static extern int DwmUpdateThumbnailProperties(IntPtr thumb, ref THUMBPROPS p);

    const uint WS_POPUP = 0x80000000;
    const uint WS_EX_TOPMOST = 0x8, WS_EX_TRANSPARENT = 0x20, WS_EX_TOOLWINDOW = 0x80,
               WS_EX_LAYERED = 0x80000, WS_EX_NOACTIVATE = 0x8000000;
    const uint WM_DESTROY = 0x2, WM_CLOSE = 0x10, WM_MOUSEACTIVATE = 0x21, WM_TIMER = 0x113;
    const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOZORDER = 0x4, SWP_NOACTIVATE = 0x10,
               SWP_NOOWNERZORDER = 0x200;
    const uint DWM_TNP_RECTDESTINATION = 0x1, DWM_TNP_OPACITY = 0x4, DWM_TNP_VISIBLE = 0x8,
               DWM_TNP_SOURCECLIENTAREAONLY = 0x10;
    const uint GW_HWNDNEXT = 2, LWA_ALPHA = 0x2;
    const int SW_HIDE = 0, SW_SHOWNOACTIVATE = 4, GWL_EXSTYLE = -20;
    static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);

    static readonly WndProcFn Proc = WndProc;   // held here so the GC never collects it
    static IntPtr hold, client, thumb, overlay;
    static RECT at;                             // where mstsc's window is, physical pixels
    static int lost;                            // ticks without an mstsc window
    static string why = "";

    public static IntPtr FindHold() { return FindWindow(ClassName, null); }
    public static void Close(IntPtr h) { PostMessage(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero); }

    // Same test as hydra7-avc's HydraMstsc.Find.
    public static IntPtr FindClient() {
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

    // The LOWEST visible window of the cursor overlay: the hold goes right under it.
    static IntPtr FindOverlay() {
        List<uint> pids = new List<uint>();
        foreach (string n in new string[] { "cursor_overlay_seat", "cursor_overlay" }) {
            foreach (Process p in Process.GetProcessesByName(n)) { pids.Add((uint)p.Id); p.Dispose(); }
        }
        if (pids.Count == 0) return IntPtr.Zero;
        IntPtr lowest = IntPtr.Zero;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (pids.Contains(pid) && IsWindowVisible(h)) lowest = h;
            return true;
        }, IntPtr.Zero);
        return lowest;
    }

    // mstsc's client area, scaled onto the whole hold. Full screen, that is
    // 1:1: mstsc has no frame, so client and window are both the panel.
    static int Place() {
        THUMBPROPS p = new THUMBPROPS();
        p.flags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        p.dest.R = at.R - at.L;
        p.dest.B = at.B - at.T;
        p.opacity = 255;
        p.visible = 1;
        p.clientOnly = 1;
        return DwmUpdateThumbnailProperties(thumb, ref p);
    }

    static bool Attach() {
        if (thumb != IntPtr.Zero) { DwmUnregisterThumbnail(thumb); thumb = IntPtr.Zero; }
        int hr = DwmRegisterThumbnail(hold, client, out thumb);
        if (hr != 0) thumb = IntPtr.Zero;
        else hr = Place();
        if (hr == 0) return true;
        why = "DWM thumbnail failed: 0x" + hr.ToString("X8");
        return false;
    }

    static void Restack() {
        if (overlay == IntPtr.Zero || !IsWindow(overlay) || !IsWindowVisible(overlay))
            overlay = FindOverlay();
        if (overlay == IntPtr.Zero) return;
        // The overlay has to be topmost for the hold to sit under it and stay topmost.
        if (((uint)GetWindowLong(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (GetWindow(overlay, GW_HWNDNEXT) != hold)
            SetWindowPos(hold, overlay, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // Every 500 ms. Deliberately no visibility test on mstsc: if the slide
    // hides it that way, that is exactly when the hold must stay up.
    static void Tick() {
        if (!IsWindow(client)) {
            client = FindClient();
            if (client == IntPtr.Zero) {
                ShowWindow(hold, SW_HIDE);
                if (++lost >= 20) { why = ""; DestroyWindow(hold); }   // 10 s: the seat was stopped
                return;
            }
            if (!Attach()) { DestroyWindow(hold); return; }
        }
        lost = 0;
        if (!IsIconic(client)) {
            RECT r;
            GetWindowRect(client, out r);
            if (r.L != at.L || r.T != at.T || r.R != at.R || r.B != at.B) {
                at = r;
                SetWindowPos(hold, IntPtr.Zero, r.L, r.T, r.R - r.L, r.B - r.T, SWP_NOZORDER | SWP_NOACTIVATE);
                if (Place() != 0 && !Attach()) { DestroyWindow(hold); return; }
            }
        }
        if (!IsWindowVisible(hold)) ShowWindow(hold, SW_SHOWNOACTIVATE);
        Restack();
    }

    static IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l) {
        try {
            if (m == WM_TIMER) { Tick(); return IntPtr.Zero; }
            if (m == WM_MOUSEACTIVATE) return new IntPtr(3);             // MA_NOACTIVATE
            if (m == WM_DESTROY) { PostQuitMessage(0); return IntPtr.Zero; }
        } catch (Exception e) {
            why = "tick: " + e.Message;
            PostQuitMessage(1);
            return IntPtr.Zero;
        }
        return DefWindowProc(h, m, w, l);
    }

    // Blocks until the hold closes. "" = clean exit, else why it could not hold.
    public static string Run() {
        SetThreadDpiAwarenessContext(new IntPtr(-4));   // per-monitor v2: physical pixels
        client = FindClient();
        if (client == IntPtr.Zero) return "no mstsc session window (TscShellContainerClass)";
        GetWindowRect(client, out at);

        WNDCLASSEX wc = new WNDCLASSEX();
        wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandle(null);
        wc.hbrBackground = GetStockObject(4);           // BLACK_BRUSH, under the thumbnail
        wc.lpszClassName = ClassName;
        if (RegisterClassEx(ref wc) == 0) {
            int e = Marshal.GetLastWin32Error();
            if (e != 1410) return "RegisterClassEx failed: " + e;   // 1410: already registered
        }
        hold = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            ClassName, "Hydra seat hold", WS_POPUP,
            at.L, at.T, at.R - at.L, at.B - at.T,
            IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (hold == IntPtr.Zero) return "CreateWindowEx failed: " + Marshal.GetLastWin32Error();
        // 254, not 255: no visible difference, but a translucent window never
        // counts as covering mstsc, so nothing in its pipeline decides it is hidden.
        SetLayeredWindowAttributes(hold, 0, 254, LWA_ALPHA);
        if (!Attach()) { DestroyWindow(hold); return why; }

        Restack();                                      // under the overlay before it shows
        ShowWindow(hold, SW_SHOWNOACTIVATE);
        Restack();
        SetTimer(hold, new IntPtr(1), 500, IntPtr.Zero);
        MSG msg;
        while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) {
            TranslateMessage(ref msg);
            DispatchMessage(ref msg);
        }
        if (thumb != IntPtr.Zero) { DwmUnregisterThumbnail(thumb); thumb = IntPtr.Zero; }
        return why;
    }
}
'@
}

# ------------------------------------------------------------------ stop -----
if ($Stop) {
    $h = [HydraSeatHold]::FindHold()
    if ($h -eq [IntPtr]::Zero) { Say 'no seat hold is up.' DarkGray; return }
    [HydraSeatHold]::Close($h)
    # UIPI drops WM_CLOSE from an unelevated shell to the elevated hold, silently.
    for ($i = 0; $i -lt 8 -and [HydraSeatHold]::FindHold() -ne [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
    }
    if ([HydraSeatHold]::FindHold() -ne [IntPtr]::Zero) {
        Say 'seat hold is still up -- run -Stop from the elevated Hydra Shell.' Yellow
    } else {
        Say 'seat hold closed.' Green
    }
    return
}

# ----------------------------------------------------------------- child -----
# Owns the window and its message loop, so it gets a process of its own: the
# loop blocks, and Ctrl+C cannot reach into it.
if ($Run) {
    $host.UI.RawUI.WindowTitle = 'Hydra seat hold'
    $why = [HydraSeatHold]::Run()
    if ($why) {
        Say "seat hold stopped: $why" Yellow
        Read-Host 'Enter closes this window' | Out-Null
    }
    return
}

# ---------------------------------------------------------------- launch -----
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say 'Run this elevated, from the Hydra Shell: mstsc and the cursor overlay are' Red
    Say 'elevated, and unelevated it may land on top of the student''s cursor.' Red
    return
}
if ([HydraSeatHold]::FindHold() -ne [IntPtr]::Zero) { Say 'seat hold is already up.' Green; return }
if ([HydraSeatHold]::FindClient() -eq [IntPtr]::Zero) {
    Say 'no mstsc session window -- bring the seat up first (hydra7-avc.ps1).' Red
    return
}

$psExe = (Get-Process -Id $PID).Path
Start-Process $psExe `
    -WindowStyle Minimized `
    -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Run'
for ($i = 0; $i -lt 60 -and [HydraSeatHold]::FindHold() -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
}
if ([HydraSeatHold]::FindHold() -eq [IntPtr]::Zero) {
    Say 'seat hold did not come up -- its minimized window says why.' Yellow
    return
}
Say 'seat hold up: the panel now shows a live copy of the seat.' Green
Say 'Switch desktops on the laptop (Ctrl+Win+Right, then back) and watch the panel.' DarkGray
Say 'Down:  .\hydra-seat-hold.ps1 -Stop   (or it goes by itself 10 s after mstsc)' Cyan
