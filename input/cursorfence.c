/* cursorfence.c -- stop seat 1's cursor walking onto the seat 2 panel.
 *
 * THE PROBLEM
 *   The seat panel is a real monitor in the console session's desktop, so the
 *   console cursor can simply walk onto it -- push right far enough and it
 *   crosses, landing on top of mirror's output. Making the RDP client window
 *   small does not help: the barrier is a DESKTOP edge, not a window.
 *
 * WHY ClipCursor DOES NOT WORK
 *   Tried first, and it looks right: one call, correct rectangle. But Windows
 *   RELEASES the clip whenever the foreground window changes, and a background
 *   helper is essentially never foreground. Re-applying on a timer leaves gaps
 *   of a few hundred milliseconds, and the cursor escapes through them -- which
 *   is exactly the leak that remained.
 *
 * WHAT WORKS
 *   A low-level mouse hook. WH_MOUSE_LL sees every movement before it reaches
 *   any window, is NOT focus-dependent, and can swallow an event outright. If a
 *   move would put the cursor on the panel, we drop it and put the cursor back
 *   on the edge -- so the pointer stops dead at the boundary instead of crossing.
 *
 *   The hook only affects THIS session. The seat's own cursor lives in its own
 *   session and never passes through here.
 *
 * BUILD:  cl /nologo /O2 cursorfence.c /link user32.lib
 * USAGE:  cursorfence.exe            -- fence off every non-primary monitor
 *         cursorfence.exe --list     -- show monitors, then exit
 *         Ctrl+C, or close the window, to remove the fence.
 *
 * NOTE: a low-level hook must not block. Everything here is arithmetic on a
 * cached rectangle -- no allocation, no I/O, no locks.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static RECT  g_allowed;          /* the only rectangle the cursor may occupy */
static BOOL  g_haveRect = FALSE;
static HHOOK g_hook     = NULL;
static UINT64 g_blocked = 0;

static BOOL CALLBACK enum_mon(HMONITOR h, HDC dc, LPRECT r, LPARAM p)
{
    MONITORINFO mi = { sizeof(mi) };
    (void)dc; (void)r;
    if (!GetMonitorInfoW(h, &mi)) return TRUE;

    BOOL primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (p) {
        wprintf(L"  %s  %ldx%ld at %ld,%ld\n",
                primary ? L"[primary]" : L"         ",
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                mi.rcMonitor.left, mi.rcMonitor.top);
        return TRUE;
    }

    /* Fence the cursor to the SMALLEST monitor -- the laptop screen. Picking by
     * size rather than by "primary" because which display Windows calls primary
     * has changed more than once on this machine, and the panel being primary
     * would invert the whole thing. */
    LONG area = (mi.rcMonitor.right - mi.rcMonitor.left)
              * (mi.rcMonitor.bottom - mi.rcMonitor.top);
    LONG best = g_haveRect
              ? (g_allowed.right - g_allowed.left) * (g_allowed.bottom - g_allowed.top)
              : 0;
    if (!g_haveRect || area < best) {
        g_allowed  = mi.rcMonitor;
        g_haveRect = TRUE;
    }
    return TRUE;
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == WM_MOUSEMOVE && g_haveRect) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)l;

        /* Injected moves are ours (or another tool's) -- never fight them, or
         * clamping and injecting would feed each other forever. */
        if (!(m->flags & LLMHF_INJECTED)) {
            LONG x = m->pt.x, y = m->pt.y;
            if (x <  g_allowed.left || x >= g_allowed.right ||
                y <  g_allowed.top  || y >= g_allowed.bottom)
            {
                /* Clamp to just inside the edge and put the cursor there, then
                 * swallow the event so nothing sees the excursion. */
                if (x <  g_allowed.left)   x = g_allowed.left;
                if (x >= g_allowed.right)  x = g_allowed.right  - 1;
                if (y <  g_allowed.top)    y = g_allowed.top;
                if (y >= g_allowed.bottom) y = g_allowed.bottom - 1;
                SetCursorPos(x, y);
                g_blocked++;
                return 1;            /* eaten */
            }
        }
    }
    return CallNextHookEx(g_hook, code, w, l);
}

static BOOL WINAPI on_ctrl(DWORD t)
{
    (void)t;
    if (g_hook) UnhookWindowsHookEx(g_hook);
    wprintf(L"\nfence removed (%llu moves blocked)\n", (unsigned long long)g_blocked);
    ExitProcess(0);
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDPIAware();      /* monitor rects in PHYSICAL pixels, like the hook */
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (argc >= 2 && _wcsicmp(argv[1], L"--list") == 0) {
        wprintf(L"monitors:\n");
        EnumDisplayMonitors(NULL, NULL, enum_mon, 1);
        return 0;
    }

    EnumDisplayMonitors(NULL, NULL, enum_mon, 0);
    if (!g_haveRect) { wprintf(L"no monitors found\n"); return 1; }

    wprintf(L"fencing the cursor to %ldx%ld at %ld,%ld\n",
            g_allowed.right - g_allowed.left, g_allowed.bottom - g_allowed.top,
            g_allowed.left, g_allowed.top);
    wprintf(L"Seat 2's cursor is in another session and is unaffected.\n");
    wprintf(L"Ctrl+C to remove the fence.\n");

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(NULL), 0);
    if (!g_hook) { wprintf(L"SetWindowsHookEx failed: %lu\n", GetLastError()); return 1; }

    /* A low-level hook needs a message pump on the thread that installed it. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_hook);
    return 0;
}
