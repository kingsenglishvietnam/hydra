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
 *
 * SEAT MODE (2026-10-10):  cursor_overlay.exe [xN] --seat B --at X,Y,W,H
 *   Draws ANOTHER seat's cursor instead of this session's: position, visibility
 *   and shape published by agent:B in Global\HydraSeat_B_cursor (see
 *   HydraSeatCursor in hydra_ipc.h), over the seat's panel at X,Y (W x H,
 *   physical pixels). Run in the CONSOLE session, over the RDP client that IS the
 *   seat's panel: the client can't draw the seat's pointer itself, and from out
 *   here the seat's Start menu, Task View and virtual desktops can't hide it.
 *   Needs no elevation or UIAccess -- build it WITHOUT the uiAccess manifest:
 *     cl /nologo /O2 /EHsc /W3 overlay\cursor_overlay.cpp /Fe:dist\cursor_overlay_seat.exe
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
#include <objbase.h>
#include <shobjidl.h> /* IVirtualDesktopManager: seat mode follows the console's desktops */
#include "../common/hydra_ipc.h"
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

static HWND    g_overlay = nullptr;
static HCURSOR g_arrow   = nullptr;
static int     g_vx, g_vy, g_vw, g_vh;   /* virtual-desktop bounds */
static volatile bool g_run = true;
static int     g_scale = 1;    /* extra multiplier on top of the user's size (default 1 = honor as-is) */
static int     g_base  = 32;   /* accessibility cursor base size, informational only --
                                 * NOT used in scaling math (see paint(): the measured
                                 * cursor bitmap size already reflects it; multiplying
                                 * by this too caused the double-count/pixellation bug) */

/* Seat mode: another seat's cursor, read from its agent's shared section. */
static bool             g_seatMode     = false;
static char             g_seat[8]      = "B";
static HydraSeatCursor* g_seatCur      = nullptr;
static DWORD            g_seatLastOpen = 0;
static uint32_t         g_seatSeq      = 0;   /* last stable seq seen        */
static DWORD            g_seatSeqTick  = 0;   /* when that seq last changed  */
static HCURSOR          g_seatCursors[HYDRA_CUR_COUNT];
static unsigned         g_seatShape    = HYDRA_CUR_ARROW;

/* Fill ci from the seat's published cursor. false = nothing to draw: section not
 * there yet, never published, or no update for a second (agent restarting, seat
 * disconnected) -- better no cursor than a frozen one. */
static bool seat_cursor(CURSORINFO& ci)
{
    if (!g_seatCur) {
        DWORD now = GetTickCount();
        if (now - g_seatLastOpen < 1000) return false;
        g_seatLastOpen = now;
        wchar_t name[128];
        hydra_cursor_name(name, 128, g_seat);
        HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!map) return false;
        void* v = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(HydraSeatCursor));
        if (!v) { CloseHandle(map); return false; }
        g_seatCur = (HydraSeatCursor*)v;        /* held for the process lifetime */
        fwprintf(stderr, L"[seat] reading %ls\n", name);
    }
    uint32_t s1 = 0, s2 = 0, vis = 0, shape = 0;
    int32_t x = 0, y = 0;
    for (int tries = 0; tries < 8; tries++) {   /* seqlock read */
        s1 = g_seatCur->seq;
        MemoryBarrier();
        x = g_seatCur->x;
        y = g_seatCur->y;
        vis = g_seatCur->visible;
        shape = g_seatCur->shape;
        MemoryBarrier();
        s2 = g_seatCur->seq;
        if (s1 == s2 && !(s1 & 1u)) break;
    }
    if (s1 != s2 || (s1 & 1u) || s1 == 0) return false;
    DWORD now = GetTickCount();
    if (s1 != g_seatSeq) { g_seatSeq = s1; g_seatSeqTick = now; }
    else if (now - g_seatSeqTick > 1000) return false;          /* stale */
    ci.flags = vis ? CURSOR_SHOWING : 0;
    g_seatShape = shape < HYDRA_CUR_COUNT ? shape : HYDRA_CUR_ARROW;
    ci.hCursor = g_seatCursors[g_seatShape];
    ci.ptScreenPos.x = g_vx + x;
    ci.ptScreenPos.y = g_vy + y;
    return true;
}

/* "X,Y,W,H" -> v[4] */
static bool parse_rect(const wchar_t* s, int* v)
{
    for (int k = 0; k < 4; k++) {
        wchar_t* end = nullptr;
        long n = wcstol(s, &end, 10);
        if (end == s) return false;
        v[k] = (int)n;
        s = end;
        if (k < 3) { if (*s != L',') return false; s++; }
    }
    return true;
}

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

/* ------------------------------------------------------------ seat mode ---
 * A cursor-sized layered window that FOLLOWS the seat's pointer:
 *   - per-pixel alpha (ULW_ALPHA), so no colour-key fringe round the edges;
 *   - the seat's own pointer image from the agent -- its scheme, colour, size
 *     and application cursors -- scaled by xN (the console's standard cursor
 *     for the seat's shape only until an image arrives);
 *   - rebuilt only when the image changes; a move is just a window move. */
static uint32_t g_img[HYDRA_CUR_MAX * HYDRA_CUR_MAX];
static unsigned g_imgW = 0, g_imgH = 0;
static unsigned g_nomW = 0, g_nomH = 0;    /* the size the seat shows it at */
static int      g_imgHotX = 0, g_imgHotY = 0;
static uint32_t g_imgSeq = 0;              /* image currently in g_img        */
static bool     g_spr = false;             /* a sprite has been built         */
static int      g_sprHotX = 0, g_sprHotY = 0;
static unsigned g_sprShape = ~0u;          /* fallback shape it was built from */
static bool     g_shown = false;
static bool     g_placed = false;
static POINT    g_sprPos = { 0, 0 };

/* Copy a new pointer image out of the section. true = g_img changed. */
static bool seat_image()
{
    if (!g_seatCur) return false;
    uint32_t s1 = g_seatCur->imgSeq;
    if (s1 == 0 || (s1 & 1u) || s1 == g_imgSeq) return false;
    MemoryBarrier();
    unsigned w = g_seatCur->imgW, h = g_seatCur->imgH;
    unsigned nw = g_seatCur->nomW, nh = g_seatCur->nomH;
    int hx = g_seatCur->hotX, hy = g_seatCur->hotY;
    if (!w || !h || w > HYDRA_CUR_MAX || h > HYDRA_CUR_MAX) return false;
    memcpy(g_img, (const void*)g_seatCur->bgra, (size_t)w * h * 4);
    MemoryBarrier();
    if (g_seatCur->imgSeq != s1) return false;     /* torn -- next frame */
    g_imgW = w; g_imgH = h; g_imgHotX = hx; g_imgHotY = hy; g_imgSeq = s1;
    g_nomW = nw ? nw : w;                          /* agents before nomW: image size */
    g_nomH = nh ? nh : h;
    static unsigned lw = 0, lnw = 0;
    if (w != lw || g_nomW != lnw) {
        lw = w; lnw = g_nomW;
        fwprintf(stderr, L"[seat] pointer image %ux%u, shown at %ux%u x%d = %ux%u\n",
                 w, h, g_nomW, g_nomH, g_scale, g_nomW * g_scale, g_nomH * g_scale);
    }
    return true;
}

/* Scale the current image (or the console's cursor for `shape`) by g_scale
 * into a premultiplied DIB and hand it to the layered window. */
static void build_sprite(unsigned shape)
{
    using namespace Gdiplus;
    Bitmap* src = nullptr;
    int hx = 0, hy = 0;
    if (g_imgSeq) {
        src = new Bitmap((INT)g_imgW, (INT)g_imgH, (INT)(g_imgW * 4),
                         PixelFormat32bppPARGB, (BYTE*)g_img);
        hx = g_imgHotX; hy = g_imgHotY;
    } else {
        HCURSOR hc = g_seatCursors[shape < HYDRA_CUR_COUNT ? shape : HYDRA_CUR_ARROW];
        ICONINFO ii{};
        if (!hc || !GetIconInfo(hc, &ii)) return;
        hx = (int)ii.xHotspot; hy = (int)ii.yHotspot;
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        src = new Bitmap((HICON)hc);
    }
    int w = (int)src->GetWidth(), h = (int)src->GetHeight();
    if (w <= 0 || h <= 0) { delete src; return; }
    /* Draw at the size the seat shows the pointer, times xN. The image may be
     * a larger original than that (its scheme file), so this is usually a
     * clean shrink rather than an enlargement. */
    int nw = g_imgSeq ? (int)g_nomW : w, nh = g_imgSeq ? (int)g_nomH : h;
    int sw = nw * g_scale, sh = nh * g_scale;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh;                   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        {
            Bitmap dst(sw, sh, sw * 4, PixelFormat32bppPARGB, (BYTE*)bits);
            Graphics g(&dst);
            g.Clear(Color(0, 0, 0, 0));
            g.SetInterpolationMode(sw == w && sh == h ? InterpolationModeNearestNeighbor
                                                      : InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            ImageAttributes ia;
            ia.SetWrapMode(WrapModeTileFlipXY);    /* no smearing in from the edges */
            g.DrawImage(src, Rect(0, 0, sw, sh), 0, 0, w, h, UnitPixel, &ia);
        }
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, dib);
        POINT zero{ 0, 0 };
        SIZE  sz{ sw, sh };
        POINT pos = g_placed ? g_sprPos : POINT{ g_vx, g_vy };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_overlay, screen, &pos, &sz, mem, &zero, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        g_spr = true;
        g_sprHotX = MulDiv(hx, sw, w);
        g_sprHotY = MulDiv(hy, sh, h);
    }
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen);
    delete src;
}

static void paint_seat()
{
    CURSORINFO ci{}; ci.cbSize = sizeof(ci);
    bool have = seat_cursor(ci);
    if (have) {
        bool fresh = seat_image();
        if (fresh || !g_spr || (!g_imgSeq && g_seatShape != g_sprShape)) {
            build_sprite(g_seatShape);
            g_sprShape = g_seatShape;
        }
    }
    if (!have || !(ci.flags & CURSOR_SHOWING) || !g_spr) {
        if (g_shown) { ShowWindow(g_overlay, SW_HIDE); g_shown = false; }
        return;
    }
    POINT pos{ ci.ptScreenPos.x - g_sprHotX, ci.ptScreenPos.y - g_sprHotY };
    if (!g_placed || pos.x != g_sprPos.x || pos.y != g_sprPos.y) {
        SetWindowPos(g_overlay, HWND_TOPMOST, pos.x, pos.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        g_sprPos = pos;
        g_placed = true;
    }
    if (!g_shown) { ShowWindow(g_overlay, SW_SHOWNOACTIVATE); g_shown = true; }
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
    /* Seat mode draws on a panel whose DPI can differ from the primary's (here a
     * 100% monitor beside a 200% laptop). Per-monitor v2 keeps every coordinate
     * physical and stops DWM bitmap-scaling the overlay on that panel. The
     * original mode keeps its system-aware behaviour. */
    bool seatArg = false;
    for (int i = 1; i < argc; i++)
        if (_wcsicmp(argv[i], L"--seat") == 0) seatArg = true;
    if (!seatArg || !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
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

    int at[4] = { 0, 0, 0, 0 };
    bool haveAt = false;
    for (int i = 1; i + 1 < argc; i++) {
        if (_wcsicmp(argv[i], L"--seat") == 0) {
            g_seatMode = true;
            WideCharToMultiByte(CP_ACP, 0, argv[i + 1], -1, g_seat, (int)sizeof(g_seat),
                                nullptr, nullptr);
        } else if (_wcsicmp(argv[i], L"--at") == 0) {
            haveAt = parse_rect(argv[i + 1], at);
        }
    }
    if (g_seatMode && (!haveAt || at[2] <= 0 || at[3] <= 0)) {
        fwprintf(stderr, L"--seat needs --at X,Y,W,H (the seat's panel, physical pixels)\n");
        return 2;
    }

    if (g_seatMode) {
        /* The seat's panel: its cursor is placed relative to this. */
        g_vx = at[0]; g_vy = at[1]; g_vw = at[2]; g_vh = at[3];
        for (unsigned i = 0; i < HYDRA_CUR_COUNT; i++)
            g_seatCursors[i] = LoadCursor(nullptr, MAKEINTRESOURCE(hydra_cursor_idc(i)));
    } else {
        /* Cover the entire virtual desktop (all monitors), so wherever the cursor is,
         * the overlay is over it. */
        g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

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
        g_vx, g_vy, g_seatMode ? 1 : g_vw, g_seatMode ? 1 : g_vh,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_overlay) { fwprintf(stderr, L"overlay create failed %lu\n", GetLastError()); return 1; }

    if (!g_seatMode) ShowWindow(g_overlay, SW_SHOWNOACTIVATE);   /* seat mode shows on first draw */

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

    if (g_seatMode)
        fwprintf(stderr, L"cursor overlay drawing seat %S's cursor over its panel "
                         L"(%d,%d %dx%d), x%d. Ctrl+C to quit.\n",
                 g_seat, g_vx, g_vy, g_vw, g_vh, g_scale);
    else
        fwprintf(stderr, L"cursor overlay running over virtual desktop "
                         L"(%d,%d %dx%d), rendering your Windows cursor+size (base %d, x%d). Ctrl+C to quit.\n",
                 g_vx, g_vy, g_vw, g_vh, g_base, g_scale);

    /* Seat mode lives over a client window that hydra7 pins to every virtual
     * desktop, so follow the console's desktop switches too. A tool window may
     * not be managed by virtual desktops at all; then this is simply a no-op. */
    IVirtualDesktopManager* vdm = nullptr;
    bool comUp = false;
    if (g_seatMode && SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        comUp = true;
        if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                                    IID_IVirtualDesktopManager, (void**)&vdm)))
            vdm = nullptr;
    }
    unsigned frame = 0;

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
        if (g_seatMode) paint_seat(); else paint();
        /* Reassert every frame, not on a slow timer: the taskbar/shell can
         * reassert ITS OWN topmost status on ordinary interaction (hover,
         * click) far more than once/second, and our old 1s gap left a window
         * where we'd lost the race -- exactly the "sometimes above, sometimes
         * not" inconsistency. SWP_NOACTIVATE+NOMOVE+NOSIZE on an
         * already-topmost window is cheap; no reason to throttle it. */
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (vdm && ++frame % 25 == 0) {            /* ~4 times a second */
            BOOL on = TRUE;
            if (SUCCEEDED(vdm->IsWindowOnCurrentVirtualDesktop(g_overlay, &on)) && !on) {
                HWND fg = GetForegroundWindow();
                GUID id;
                if (fg && SUCCEEDED(vdm->GetWindowDesktopId(fg, &id)))
                    vdm->MoveWindowToDesktop(g_overlay, id);
            }
        }
        Sleep(11);
    }

    if (vdm) vdm->Release();
    if (comUp) CoUninitialize();
    DestroyWindow(g_overlay);
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
