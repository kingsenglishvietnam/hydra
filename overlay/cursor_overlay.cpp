/* cursor_overlay.cpp  --  draw a visible cursor sprite at the real cursor
 * position, for displays where the hardware cursor isn't scanned out (IddCx
 * virtual monitors, where the pointer is functional but invisible).
 *
 * THE IDEA
 *   The cursor already works -- clicks land, hover fires, drags drag. It's just
 *   not being *painted* on the IDD panel. So we paint it ourselves: a transparent,
 *   topmost, CLICK-THROUGH overlay window that reads GetCursorPos() and blits a
 *   cursor bitmap there every frame. No driver change, no session work.
 *
 *   Click-through is the key: WS_EX_TRANSPARENT | WS_EX_LAYERED means every click
 *   passes straight through the overlay to the real window underneath, so the
 *   working input path is untouched -- we only add pixels, never intercept.
 *
 * WORKS IF: GetCursorPos() reports the true position on the invisible-cursor
 * panel (it should -- that's the same position the OS uses to land clicks).
 * FAILS IF: that panel's cursor position is disconnected from the global cursor
 * (then the sprite tracks wrong). Running this tells you which -- instantly.
 *
 * BUILD:  cl /O2 /EHsc cursor_overlay.cpp
 * RUN:    cursor_overlay.exe [xN]       (renders the USER'S chosen cursor+size;
 *                                   optional xN = extra size multiplier for distance)
 *   Esc or Ctrl+C in the console to quit. It draws an arrow at the cursor.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>   /* _wtoi (MSVC doesn't pull it in via windows.h) */
#include <objidl.h>   /* PROPID etc. (MinGW's gdiplus headers need this first) */
#include <gdiplus.h>  /* high-quality (bicubic) cursor scaling */
#pragma comment(lib, "gdiplus.lib")

static HWND    g_overlay = nullptr;
static HCURSOR g_arrow   = nullptr;
static int     g_vx, g_vy, g_vw, g_vh;   /* virtual-desktop bounds */
static volatile bool g_run = true;
static int     g_scale = 1;    /* extra multiplier on top of the user's size (default 1 = honor as-is) */
static int     g_base  = 32;   /* accessibility cursor base size, informational only --
                                 * NOT used in scaling math (see paint(): the measured
                                 * cursor bitmap size already reflects it; multiplying
                                 * by this too caused the double-count/pixellation bug) */

/* Read the user's accessibility cursor base size from the registry. Windows
 * stores the chosen pointer size as HKCU\Control Panel\Cursors\CursorBaseSize
 * (32 = default/100%; larger = the accessibility size slider). We use it to
 * scale the rendered cursor to match what the user picked, since a cursor handle
 * fetched in one DPI context won't always carry the accessibility size. */
static int cursor_base_size()
{
    int base = 32;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_READ, &k)
        == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"CursorBaseSize", nullptr, &type,
                             (LPBYTE)&v, &sz) == ERROR_SUCCESS
            && type == REG_DWORD && v >= 32 && v <= 256) {
            base = (int)v;
        }
        RegCloseKey(k);
    }
    return base;
}

/* Redraw: clear to the transparent color key, then draw the user's ACTUAL cursor
 * (their chosen scheme, shape, and accessibility size) at its position. */
static void paint()
{
    CURSORINFO ci{}; ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
        /* Cursor hidden (e.g. text-input field hides it) -- clear the overlay. */
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, g_vw, g_vh);
        HGDIOBJ ob = SelectObject(mem, bmp);
        HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
        RECT full{ 0, 0, g_vw, g_vh }; FillRect(mem, &full, key); DeleteObject(key);
        POINT s0{0,0}, d0{g_vx,g_vy}; SIZE sz0{g_vw,g_vh};
        UpdateLayeredWindow(g_overlay, screen, &d0, &sz0, mem, &s0,
                            RGB(255,0,255), nullptr, ULW_COLORKEY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
        return;
    }

    /* ci.ptScreenPos is the HOTSPOT location (where the pointer actually points).
     * DrawIconEx positions by the image's top-left, so we must subtract the
     * cursor's hotspot -- and because we resize the cursor, subtract the hotspot
     * SCALED by the same ratio, or the tip drifts down-right as size grows. */
    int x = ci.ptScreenPos.x - g_vx;
    int y = ci.ptScreenPos.y - g_vy;

    /* Measure the cursor's ACTUAL current pixel size first -- this is ground
     * truth, and on a DPI-scaled / accessibility-large-cursor setup it already
     * reflects that scaling (confirmed: measured 96px when accessibility base
     * was 80, nowhere near a flat 32). The old code multiplied SM_CXCURSOR by
     * g_base too, which double-counted the accessibility size when SM_CXCURSOR
     * ITSELF already reflects it on this machine -- that's what caused visible
     * pixellation even at "x1" (no extra multiplier requested). Fix: use the
     * measured size as-is, and let g_scale be the only extra multiplier. */
    int hotx = 0, hoty = 0;
    int native = GetSystemMetrics(SM_CXCURSOR);   /* fallback only, if measure fails */
    ICONINFO ii{};
    if (GetIconInfo(ci.hCursor, &ii)) {
        hotx = (int)ii.xHotspot;
        hoty = (int)ii.yHotspot;
        BITMAP bm{};
        HBITMAP probe = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
        if (probe && GetObject(probe, sizeof(bm), &bm) && bm.bmWidth > 0) {
            native = bm.bmWidth;   /* cursors are square; width is unambiguous
                                     * (height needs /2 for mono AND/XOR masks) */
        }
        static bool loggedSize = false;
        if (!loggedSize) {
            loggedSize = true;
            int measH = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
            fwprintf(stderr, L"[diag] measured cursor %dx%d px; using that as the "
                             L"scale reference (not SM_CXCURSOR/g_base) -- x%d "
                             L"multiplier requested\n", bm.bmWidth, measH, g_scale);
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    }

    /* target: the cursor's actual measured size, times ONLY the explicit extra
     * multiplier. At the default g_scale=1 this equals `native` exactly, so we
     * take the plain DrawIconEx 1:1 path below -- zero resampling, pixel-exact
     * match to what Windows itself is currently showing. */
    int target = native * g_scale;

    /* Scale the hotspot to the target size and shift the draw origin by it. */
    int drawx = x - MulDiv(hotx, target, native);
    int drawy = y - MulDiv(hoty, target, native);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, g_vw, g_vh);
    HGDIOBJ oldbmp = SelectObject(mem, bmp);

    HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
    RECT full{ 0, 0, g_vw, g_vh };
    FillRect(mem, &full, key);
    DeleteObject(key);

    /* DrawIconEx's stretch is capped by the cursor resource's native pixels and
     * gets blurry/clamped when enlarged. To get a genuinely large, clean cursor,
     * resize the cursor with CopyImage to the target size first, then draw it
     * 1:1. CopyImage resamples from the source properly. */
    if (target <= native) {
        DrawIconEx(mem, drawx, drawy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    } else {
        /* Smooth enlarge: render the cursor into a GDI+ bitmap at native size,
         * then draw it scaled with high-quality (bicubic) interpolation so big
         * cursors are smooth, not blocky. */
        Gdiplus::Bitmap src((HICON)ci.hCursor);
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        Gdiplus::Status st = g.DrawImage(&src, drawx, drawy, target, target);
        if (st != Gdiplus::Ok) {
            /* Fallback if GDI+ can't wrap this cursor: stretched DrawIconEx. */
            DrawIconEx(mem, drawx, drawy, ci.hCursor, target, target, 0, nullptr, DI_NORMAL);
        }
    }

    POINT src{ 0, 0 };
    POINT dst{ g_vx, g_vy };
    SIZE  sz{ g_vw, g_vh };
    UpdateLayeredWindow(g_overlay, screen, &dst, &sz, mem, &src,
                        RGB(255, 0, 255), nullptr, ULW_COLORKEY);

    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static BOOL WINAPI ctrl(DWORD) { g_run = false; if (g_overlay) PostMessageW(g_overlay, WM_CLOSE, 0, 0); return TRUE; }

int wmain(int argc, wchar_t** argv)
{
    SetProcessDPIAware();
    SetConsoleCtrlHandler(ctrl, TRUE);
    g_arrow = LoadCursor(nullptr, IDC_ARROW);

    /* Start GDI+ (for high-quality cursor scaling). */
    Gdiplus::GdiplusStartupInput gdipIn;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipIn, nullptr);

    /* By default, render exactly what Windows would: the user's chosen cursor
     * scheme AND accessibility size. Optional arg is an EXTRA multiplier on top
     * of that (e.g. for a distant classroom screen): cursor_overlay.exe [xN]. */
    g_base = cursor_base_size();       /* honor the user's pointer-size setting */
    if (argc >= 2) {
        int s = _wtoi(argv[1]);
        if (s >= 1 && s <= 20) g_scale = s;
    }

    /* Cover the entire virtual desktop (all monitors), so wherever the cursor is,
     * the overlay is over it. */
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hydra_cursor_overlay";
    RegisterClassW(&wc);

    /* LAYERED + TRANSPARENT = click-through; TOPMOST = above everything;
     * NOACTIVATE = never takes focus; TOOLWINDOW = no taskbar entry. */
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP,
        g_vx, g_vy, g_vw, g_vh,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_overlay) { fwprintf(stderr, L"overlay create failed %lu\n", GetLastError()); return 1; }

    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);

    /* Report whether this process actually has UIAccess -- the whole point of the
     * manifest+sign+trusted-path+elevation dance. If this prints 0, the cursor
     * will NOT rise above the Start menu, and one of the four conditions is off
     * (usually: not run from Program Files, or signature not trusted). */
    {
        HANDLE tok = nullptr;
        DWORD uiaccess = 0, len = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            GetTokenInformation(tok, TokenUIAccess, &uiaccess, sizeof(uiaccess), &len);
            CloseHandle(tok);
        }
        fwprintf(stderr, L"UIAccess = %lu  (1 = can draw above Start menu; "
                         L"0 = check manifest/sign/trusted-path/elevation)\n",
                 uiaccess);
    }

    fwprintf(stderr, L"cursor overlay running over virtual desktop "
                     L"(%d,%d %dx%d), rendering your Windows cursor+size (base %d, x%d). Ctrl+C to quit.\n",
             g_vx, g_vy, g_vw, g_vh, g_base, g_scale);

    /* ~90 Hz redraw loop. Cheap: one bitmap blit per frame.
     *
     * WS_EX_TOPMOST at creation only means "eligible for the topmost band" --
     * it is NOT a standing guarantee of front-of-band position. Other topmost
     * windows (an elevated terminal asserting its own z-order, the shell
     * recreating the taskbar, etc.) can push us behind within that band, and
     * since nothing re-asserts afterward, we'd just stay behind -- matching
     * exactly the "vanishes on terminal focus, stays gone until restart" bug.
     * Fix: periodically re-assert HWND_TOPMOST so any drift self-corrects
     * within ~1s, instead of only ever being topmost once at startup. */
    MSG msg;
    while (g_run) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_run = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        paint();
        /* Reassert every frame, not on a slow timer: the taskbar/shell can
         * reassert ITS OWN topmost status on ordinary interaction (hover,
         * click) far more than once/second, and our old 1s gap left a window
         * where we'd lost the race -- exactly the "sometimes above, sometimes
         * not" inconsistency. SWP_NOACTIVATE+NOMOVE+NOSIZE on an
         * already-topmost window is cheap; no reason to throttle it. */
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Sleep(11);
    }

    DestroyWindow(g_overlay);
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
