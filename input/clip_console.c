/* clip_console.c  --  multiseat input router, seat A pointer confinement (v3)
 *
 * Runs in the CONSOLE session (seat A). Installs a WH_MOUSE_LL low-level
 * mouse hook and clamps the pointer to one monitor, so seat A's cursor can
 * never wander onto the monitor showing an extra seat's RDP window.
 *
 * v2 -> v3:
 *   - INSTANT SECURE-DESKTOP RECOVERY. LL hooks don't run on the secure
 *     desktop (UAC, lock screen, Ctrl-Alt-Del), so the pointer can cross
 *     monitors during one; v2 retrieved it on the next watchdog tick (up to
 *     2 s). v3 registers a WinEvent hook for EVENT_SYSTEM_DESKTOPSWITCH: the
 *     moment the desktop switches back, the hook is reasserted and the
 *     cursor snapped inside -- milliseconds instead of seconds. (The
 *     crossing itself still can't be *prevented* from user mode; the secure
 *     desktop exists precisely so nothing like us can interfere with it.)
 *   - SELF HANG-WATCHDOG. If this process's message pump wedges, Windows
 *     drops the LL hook (fail-open: seat A's mouse keeps working, unclamped)
 *     but the process still looks alive, so respawn.exe would never step in.
 *     A watchdog thread checks that the 2 s timer keeps ticking; after
 *     HANG_MS of stillness it terminates its own process nonzero and respawn
 *     starts a working clamp.
 *   - QUICKEDIT DISABLED so a drag-select in this console can't block our
 *     (rare) log writes and stall the pump into exactly that state.
 *   - Exit-code contract: 2 = configuration error (bad monitor index) --
 *     respawn.exe will NOT restart; 0 = deliberate stop (Ctrl-C, or the
 *     no-argument monitor listing).
 *
 * (v1 -> v2 recap: zero-gap preventive rehook every 2 s -- Windows silently
 * removes timed-out LL hooks and offers no way to ask, so we don't ask;
 * topology tracking by monitor DEVICE NAME with suspend/resume when the
 * chosen panel unplugs; dead-hook evidence logging.)
 *
 * Mechanics (unchanged since v1): a WH_MOUSE_LL hook cannot *edit* a move in
 * flight, but it can BLOCK it. Out-of-bounds move -> return 1 to swallow it,
 * then SetCursorPos() to the nearest point inside the rect, so the cursor
 * slides along the monitor edge exactly like ClipCursor feels. The
 * SetCursorPos re-enters this hook as an injected move (LLMHF_INJECTED) at
 * in-bounds coordinates and passes straight through. Injected moves are
 * never repositioned -- only blocked if out of bounds -- so recursion is
 * impossible by construction, and third-party injectors can't teleport the
 * cursor out either.
 *
 * Session scope: LL hooks are per-session. This sees only the console
 * session's pointer; the extra seats' cursors (inside their own sessions,
 * rendered through mstsc windows) are untouched.
 *
 * DPI: MSLLHOOKSTRUCT coordinates are physical pixels, but GetMonitorInfo
 * in a DPI-unaware process returns virtualized rects -- on mixed-DPI setups
 * (200% panel + 100% external) the two disagree. We opt into Per-Monitor-V2
 * awareness at startup so monitor rects, hook coords and SetCursorPos all
 * speak physical pixels.
 *
 * Build (x64 Native Tools Command Prompt -- no SDK needed, pure Win32):
 *   cl /O2 clip_console.c
 *
 * Run (console session):
 *   clip_console.exe            -> lists monitors + rectangles
 *   clip_console.exe 0          -> confine seat A's pointer to monitor 0
 *
 * Ctrl-C releases. If the process dies any other way, the OS removes the
 * hook automatically -- a crashed clamp cannot wedge the pointer.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")

#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef EVENT_SYSTEM_DESKTOPSWITCH
#define EVENT_SYSTEM_DESKTOPSWITCH 0x0020
#endif

#define WATCHDOG_MS 2000
#define HANG_MS     10000    /* 5 missed watchdog ticks -> pump is wedged */
#define MAX_MON     16
#define EXIT_CONFIG 2

typedef struct {
    RECT    rc;
    wchar_t dev[CCHDEVICENAME];
} Mon;

static Mon g_mon[MAX_MON];
static int g_nmon = 0;

static RECT    g_rect;                 /* allowed rect, physical pixels      */
static wchar_t g_dev[CCHDEVICENAME];   /* identity of the chosen monitor     */
static int     g_suspended = 0;        /* monitor gone -> full desktop       */
static HHOOK   g_hook = NULL;
static DWORD   g_tid  = 0;             /* main thread id, for Ctrl-C quit    */

/* Liveness accounting. Hook proc, timer proc, WinEvent proc and wndproc are
 * all dispatched by the main thread's message pump, so these are race-free
 * among themselves; g_alive is additionally read by the watchdog thread
 * (monotonic change detection only, so a plain volatile read suffices). */
static unsigned long  g_seen      = 0; /* hook callbacks observed            */
static unsigned long  g_seen_snap = 0;
static POINT          g_lastpos;
static volatile LONG  g_alive     = 0; /* pump liveness for hang watchdog    */

static BOOL CALLBACK mon_cb(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp) {
    (void)hdc; (void)rc; (void)lp;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (g_nmon < MAX_MON && GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) {
        g_mon[g_nmon].rc = mi.rcMonitor;
        lstrcpynW(g_mon[g_nmon].dev, mi.szDevice, CCHDEVICENAME);
        g_nmon++;
    }
    return TRUE;
}

static void enum_monitors(void) {
    g_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
}

/* Per-Monitor-V2 DPI awareness. Resolved via GetProcAddress so this compiles
 * and runs on any SDK/OS combo; the awareness context is just a pseudo-handle
 * ((DPI_AWARENESS_CONTEXT)-4 == PER_MONITOR_AWARE_V2). */
static void dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                      "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(INT_PTR)-4);
    else   SetProcessDPIAware();       /* pre-1703 fallback: system aware */
}

static void disable_quickedit(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
        SetConsoleMode(in, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
}

static LONG clamp_x(LONG x) {
    return x <  g_rect.left  ? g_rect.left
         : x >= g_rect.right ? g_rect.right - 1 : x;
}
static LONG clamp_y(LONG y) {
    return y <  g_rect.top    ? g_rect.top
         : y >= g_rect.bottom ? g_rect.bottom - 1 : y;
}

static void snap_cursor_in(void) {
    POINT p;
    if (GetCursorPos(&p)) {
        LONG cx = clamp_x(p.x), cy = clamp_y(p.y);
        if (cx != p.x || cy != p.y) SetCursorPos(cx, cy);
    }
}

/* Re-derive g_rect from the current topology. Finds the chosen monitor by
 * device name; if it's gone, clamps to the full virtual screen (suspended)
 * until it returns. Logs only when something actually changed, so spammy
 * WM_SETTINGCHANGE broadcasts cost nothing. */
static void rebind(const char *why) {
    enum_monitors();
    RECT nr;
    int  ok = 0;
    for (int i = 0; i < g_nmon; i++)
        if (wcscmp(g_mon[i].dev, g_dev) == 0) { nr = g_mon[i].rc; ok = 1; break; }
    if (!ok) {
        nr.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
        nr.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
        nr.right  = nr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        nr.bottom = nr.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    int changed = (ok == g_suspended) ||           /* suspend/resume flip */
                  memcmp(&nr, &g_rect, sizeof(nr)) != 0;
    if (!changed) return;

    g_rect      = nr;
    g_suspended = !ok;
    if (!ok)
        fprintf(stderr, "[clip] monitor %ls gone (%s); clamp suspended until "
                        "it returns\n", g_dev, why);
    else
        fprintf(stderr, "[clip] %ls bound to (%ld,%ld)-(%ld,%ld) (%s)\n",
                g_dev, nr.left, nr.top, nr.right, nr.bottom, why);
    snap_cursor_in();
}

/* Hot path. In-bounds move: one bounds check, no syscalls, pass through.
 * Only escape attempts pay for a SetCursorPos. */
static LRESULT CALLBACK mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        g_seen++;                                /* liveness for the watchdog */
        if (wParam == WM_MOUSEMOVE) {
            const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lParam;
            LONG x = ms->pt.x, y = ms->pt.y;

            if (x < g_rect.left || x >= g_rect.right ||
                y < g_rect.top  || y >= g_rect.bottom) {

                /* Physical move heading out of bounds: park the cursor at
                 * the nearest in-bounds point so it slides along the edge.
                 * Injected moves are only ever blocked (our own SetCursorPos
                 * is injected AND in-bounds, so it never lands here -- that
                 * is the recursion break). */
                if (!(ms->flags & LLMHF_INJECTED))
                    SetCursorPos(clamp_x(x), clamp_y(y));
                return 1;                        /* swallow the escape */
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* Zero-gap rehook: install the new hook BEFORE removing the old one. Both
 * clamp during the overlap, and CallNextHookEx ignores its hhk argument for
 * LL hooks, so the swap is harmless either way. */
static void rehook(void) {
    HHOOK nh = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc,
                                 GetModuleHandleW(NULL), 0);
    if (nh) {
        HHOOK old = g_hook;
        g_hook = nh;
        if (old) UnhookWindowsHookEx(old);
    } else {
        fprintf(stderr, "[clip] rehook failed (err %lu); keeping old hook\n",
                GetLastError());
    }
}

/* Watchdog tick. Four duties: prove the pump alive, re-check topology,
 * preventively rehook, and retrieve any cursor that escaped while hooks
 * weren't running. */
static void CALLBACK watchdog(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)hwnd; (void)msg; (void)id; (void)time;

    InterlockedIncrement(&g_alive);              /* hang watchdog heartbeat */

    /* topology double-check (belt for a missed WM_DISPLAYCHANGE) */
    rebind("watchdog");

    rehook();

    /* dead-hook evidence: the pointer moved but no callback fired. Either
     * the old hook had been silently dropped, or a secure desktop carried
     * the pointer while hooks were suspended and the DESKTOPSWITCH event was
     * missed. Both get the same medicine: we just reinstalled, now retrieve
     * the cursor. */
    POINT p;
    if (GetCursorPos(&p)) {
        int moved = (p.x != g_lastpos.x || p.y != g_lastpos.y);
        if (moved && g_seen == g_seen_snap) {
            fprintf(stderr, "[clip] pointer moved unseen (dead hook or secure "
                            "desktop); hook reinstalled, cursor re-clamped\n");
            snap_cursor_in();
        }
        g_lastpos = p;
    }
    g_seen_snap = g_seen;
}

/* Desktop-switch WinEvent. Fires when switching TO the secure desktop and
 * again when switching BACK. We can't act on the former (hooks and
 * SetCursorPos don't work over there -- by design) but the latter is the
 * exact instant the pointer becomes governable again, so reassert
 * immediately instead of waiting up to a watchdog period. Both fires get the
 * same idempotent treatment; on the switch-away, the calls just no-op. */
static void CALLBACK on_desktop_switch(HWINEVENTHOOK hh, DWORD ev, HWND hwnd,
                                       LONG idObject, LONG idChild,
                                       DWORD tid, DWORD etime) {
    (void)hh; (void)ev; (void)hwnd; (void)idObject; (void)idChild;
    (void)tid; (void)etime;
    rehook();
    snap_cursor_in();
}

/* Hidden top-level window: broadcast sink for display changes. (Message-only
 * windows don't receive broadcasts; this must be top-level, just invisible.) */
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DISPLAYCHANGE || m == WM_SETTINGCHANGE || m == WM_DPICHANGED)
        rebind("display change");
    return DefWindowProcW(h, m, w, l);
}

/* Watches the pump. When healthy, the 2 s timer ticks g_alive; if it goes
 * still for HANG_MS the pump is wedged -- the LL hook has already failed
 * open (Windows drops it, seat A's mouse works unclamped) but the process
 * still looks alive, so nothing would ever fix it. Die nonzero and let
 * respawn.exe start a working clamp. TerminateProcess, not ExitProcess,
 * which can deadlock behind a wedged thread's locks. */
static DWORD WINAPI hang_watchdog(LPVOID arg) {
    (void)arg;
    LONG      last  = g_alive;
    ULONGLONG since = GetTickCount64();
    for (;;) {
        Sleep(1000);
        LONG      now = g_alive;
        ULONGLONG t   = GetTickCount64();
        if (now != last) { last = now; since = t; continue; }
        if (t - since >= HANG_MS) {
            fprintf(stderr, "[clip] message pump wedged for %lu ms; dying "
                            "for respawn\n", (unsigned long)(t - since));
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    /* not reached */
    return 0;
}

static BOOL WINAPI on_ctrl(DWORD type) {
    PostThreadMessage(g_tid, WM_QUIT, 0, 0);
    /* Ctrl-C / Ctrl-Break: we handle it -- the message loop exits, unhooks,
     * prints. Close / logoff / shutdown: post anyway but let the default
     * handler proceed; the OS drops an LL hook when its process dies, so
     * there is nothing we are required to clean up. */
    return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT);
}

int main(int argc, char **argv) {
    dpi_aware();                       /* BEFORE enumerating: physical rects */
    disable_quickedit();

    enum_monitors();
    for (int i = 0; i < g_nmon; i++)
        printf("  monitor %d: (%ld,%ld)-(%ld,%ld)  %ls\n", i,
               g_mon[i].rc.left, g_mon[i].rc.top,
               g_mon[i].rc.right, g_mon[i].rc.bottom, g_mon[i].dev);

    if (argc <= 1) {
        printf("re-run with a monitor index to confine seat A to it\n");
        return 0;                      /* deliberate stop: respawn stays down */
    }

    /* Accept either a monitor index ("0") or a device name ("\\.\DISPLAY2").
     * Device names are the stable identifier used across Hydra (config, mirror)
     * and survive replug/rescale; the index is kept for the standalone path. */
    int idx = -1;
    if (argv[1][0] == '\\') {
        wchar_t want[CCHDEVICENAME];
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, want, CCHDEVICENAME);
        for (int i = 0; i < g_nmon; i++)
            if (wcscmp(g_mon[i].dev, want) == 0) { idx = i; break; }
        if (idx < 0) {
            printf("monitor '%s' not found\n", argv[1]);
            return EXIT_CONFIG;        /* config error: respawn stays down */
        }
    } else {
        idx = atoi(argv[1]);
        if (idx < 0 || idx >= g_nmon) {
            printf("only %d monitor(s) found\n", g_nmon);
            return EXIT_CONFIG;        /* config error: respawn stays down */
        }
    }
    g_rect = g_mon[idx].rc;
    lstrcpynW(g_dev, g_mon[idx].dev, CCHDEVICENAME);
    GetCursorPos(&g_lastpos);

    /* The hook runs synchronously in this session's mouse path; make sure a
     * busy box can't starve the thread hosting it (a starved LL hook = laggy
     * pointer for all of seat A, and repeated timeouts get it silently
     * unhooked -- the very thing the watchdog exists to mop up). */
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    /* Create this thread's message queue BEFORE installing the Ctrl handler,
     * so a very early Ctrl-C has somewhere to post WM_QUIT. */
    g_tid = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"clip_console_sink";
    HWND sink = NULL;
    if (RegisterClassW(&wc))
        sink = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                               NULL, NULL, wc.hInstance, NULL);
    if (!sink)
        fprintf(stderr, "[clip] no broadcast window; relying on watchdog for "
                        "display changes\n");

    /* v3: instant reassert on return from UAC / lock screen / Ctrl-Alt-Del.
     * OUTOFCONTEXT -> delivered through our message pump, no DLL needed. If
     * registration fails, the watchdog still covers it at 2 s granularity. */
    if (!SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
                         NULL, on_desktop_switch, 0, 0, WINEVENT_OUTOFCONTEXT))
        fprintf(stderr, "[clip] no desktop-switch hook; secure-desktop "
                        "recovery falls back to the 2 s watchdog\n");

    SetTimer(NULL, 0, WATCHDOG_MS, watchdog);
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc,
                               GetModuleHandleW(NULL), 0);
    if (!g_hook)
        fprintf(stderr, "[clip] initial hook install failed (err %lu); "
                        "watchdog will keep retrying\n", GetLastError());

    snap_cursor_in();                  /* pull in a cursor already astray */
    printf("[clip] confining seat A to monitor %d (%ls); Ctrl-C to release\n",
           idx, g_dev);

    /* LL hook callbacks, the watchdog TIMERPROC, the WinEvent proc and the
     * sink's wndproc are all delivered through this pump. GetMessage blocks
     * -- between events nothing runs but the 2 s watchdog. */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    printf("\n[clip] released\n");
    return 0;
}
