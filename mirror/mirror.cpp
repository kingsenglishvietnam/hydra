/* mirror.cpp  --  Hydra per-seat presenter (RDP-transport replacement)
 *
 * Runs in the CONSOLE session (seat A). Opens the shared surface that `iddseat`
 * publishes for one seat's virtual monitor (by name, via hydra_ipc.h) and
 * presents it fullscreen + chromeless on that seat's physical panel. This is the
 * piece that deletes mstsc: session B renders into its virtual monitor, this
 * copies the result straight onto the glass. No encode, no decode, no network,
 * no viewer window to lose focus.
 *
 * WHY THIS IS SAFE TO BE "DUMB"
 *   Input isolation is handled entirely by the v3 router + agent: seat B's
 *   keystrokes are captured in session A and SendInput'd into session B. So this
 *   surface never needs focus and never receives input — it is a picture of
 *   session B, nothing more. That decoupling is why a fullscreen blit can replace
 *   an interactive RDP window.
 *
 * BOTH ORIGINAL FILL-INS ARE DONE
 *   HYDRA-TODO(ipc)  -> OpenMeta()/OpenSurface() implement the consumer half of
 *                       hydra_ipc.h: open Global\HydraSeat_<seat>_meta, read the
 *                       LUID + dims, open Global\HydraSeat_<seat>_surf by name.
 *   HYDRA-TODO(luid) -> the device is created on the LUID the metadata reports,
 *                       so it shares iddseat's GPU and the surface open is
 *                       zero-copy (hydrad pins that GPU as the physical output's).
 *
 * BUILD (x64 Native Tools Command Prompt): cl /O2 /EHsc mirror.cpp
 *   Needs D3D11 + DXGI. Not MinGW-buildable; not compiled in the dev container.
 *   The IPC contract it depends on is unit-tested natively (tests/).
 *
 * RUN: mirror.exe <seat-name> <\\.\DISPLAYn>
 *   e.g. mirror.exe B "\\.\DISPLAY2"
 *   Normally launched (into the console session) and supervised by hydrad.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <stdio.h>
#include <string>

#include "../common/hydra_ipc.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

/* --------------------------------------------------------------------------- *
 * Target physical monitor, addressed by device name (same convention as
 * clip_console: indices renumber on replug, device names don't).
 * --------------------------------------------------------------------------- */
struct MonMatch { RECT rc; bool found; };
static MonMatch     g_match;
static const wchar_t* g_wantDev;

static BOOL CALLBACK mon_cb(HMONITOR h, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, (MONITORINFO*)&mi) && wcscmp(mi.szDevice, g_wantDev) == 0)
    {
        g_match.rc = mi.rcMonitor;
        g_match.found = true;
    }
    return TRUE;
}

static MonMatch find_monitor(const wchar_t* dev)
{
    g_match = {}; g_wantDev = dev;
    EnumDisplayMonitors(nullptr, nullptr, mon_cb, 0);
    return g_match;
}

/* --------------------------------------------------------------------------- *
 * Chromeless fullscreen window on the target monitor. Borderless popup, no DXGI
 * exclusive fullscreen (flip-model borderless avoids mode-switch flicker and
 * coexists with the console owning the panel). NOACTIVATE so it never steals
 * focus from seat A.
 * --------------------------------------------------------------------------- */
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

/* Ordinary resizable window, for viewing a seat on the console's own screen.
 *
 * The seat's desktop is a fixed 1920x1080 and mirror already scales whatever it
 * receives to the surface it is given -- that is how it fills a 6720x3780 panel.
 * Pointing a second instance at a normal window therefore gives a scaled-up view
 * of the seat WITHOUT the RDP client's limitation: mstsc's "smart sizing" only
 * ever scales DOWN, so its window is capped at session size plus chrome and
 * cannot be enlarged. Mirror has no such restriction.
 *
 * Deliberately NOT topmost and NOT click-through: this one is a window you look
 * at and place like any other, unlike the panel instance which must stay above
 * everything. */
/* ---------------------------------------------------------------------------
 * INPUT FORWARDING (windowed view only)
 *
 * The view window is otherwise just a picture. Forwarding mouse and keyboard
 * from it into the seat's session makes it a working remote view -- and mirror
 * cannot inject directly, because SendInput only reaches the caller's own
 * session. seatB_agent already lives in the seat's session and does exactly
 * that job, so we speak the same 9-byte wire protocol the router uses and let
 * the agent do the injecting.
 *
 * Absolute positioning is what makes this natural: the window knows where a
 * click landed in FRAME coordinates, so it maps straight to 0..65535 across the
 * seat's desktop regardless of how the window has been resized. That is exactly
 * what WIRE_M_ABS was built for.
 *
 * Records MUST byte-match WireEvent in seat_router.c / seatB_agent.c.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
struct WireEvent {
    unsigned char  kind;   /* 'K' keyboard, 'M' mouse */
    unsigned short a;      /* K: scancode      ; M: button state + flags */
    unsigned short b;      /* K: key state     ; M: wheel delta          */
    short          dx;     /* M: dx, or absolute x with WIRE_M_ABS       */
    short          dy;     /* M: dy, or absolute y with WIRE_M_ABS       */
};
#pragma pack(pop)

#define WIRE_M_ABS      0x1000
#define I_KEY_UP        0x01
#define I_KEY_E0        0x02
#define I_M_LEFTDOWN    0x001
#define I_M_LEFTUP      0x002
#define I_M_RIGHTDOWN   0x004
#define I_M_RIGHTUP     0x008
#define I_M_MIDDLEDOWN  0x010
#define I_M_MIDDLEUP    0x020
#define I_M_WHEEL       0x400

static SOCKET g_inSock = INVALID_SOCKET;
static int    g_inPort = 0;

static void input_connect(void)
{
    if (g_inSock != INVALID_SOCKET || g_inPort == 0) return;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u_short)g_inPort);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) { closesocket(s); return; }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    g_inSock = s;
    fwprintf(stderr, L"[mirror] input forwarding connected to router inject port %d\n", g_inPort);
    fflush(stderr);
}

static void wire_send(const WireEvent& e)
{
    if (g_inSock == INVALID_SOCKET) { input_connect(); if (g_inSock == INVALID_SOCKET) return; }
    if (send(g_inSock, (const char*)&e, (int)sizeof(e), 0) != (int)sizeof(e)) {
        closesocket(g_inSock);
        g_inSock = INVALID_SOCKET;   /* reconnect on the next event */
    }
}

/* Window client point -> 0..65535 across the seat's desktop. Uses the CLIENT
 * rect, so the mapping stays correct at any window size -- resize the window and
 * clicks still land where you point. */
/* Undo this session's left-handed swap before sending.
 *
 * Windows applies SwapMouseButtons when turning a physical press into a window
 * message, so on a left-handed console the PHYSICAL right button arrives as
 * WM_LBUTTONDOWN. Forwarding that as "left" means the seat's session -- which
 * also has the swap on -- applies it a SECOND time on injection, and the click
 * comes out as the wrong button.
 *
 * seat B's own mouse and mstsc are unaffected because they carry physical button
 * state, never the logical one. So do the same: convert back to physical here,
 * and let the seat's own setting do the single swap it expects.
 *
 * Read fresh each time rather than cached -- the setting can change while we
 * run, and it costs nothing. */
static unsigned short unswap_buttons(unsigned short b)
{
    if (!GetSystemMetrics(SM_SWAPBUTTON)) return b;
    unsigned short out = (unsigned short)(b & ~(I_M_LEFTDOWN | I_M_LEFTUP |
                                                I_M_RIGHTDOWN | I_M_RIGHTUP));
    if (b & I_M_LEFTDOWN)  out |= I_M_RIGHTDOWN;
    if (b & I_M_LEFTUP)    out |= I_M_RIGHTUP;
    if (b & I_M_RIGHTDOWN) out |= I_M_LEFTDOWN;
    if (b & I_M_RIGHTUP)   out |= I_M_LEFTUP;
    return out;
}

static void send_mouse_abs(HWND hwnd, int cx, int cy, unsigned short buttons, short wheel)
{
    buttons = unswap_buttons(buttons);
    RECT rc{}; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (cx < 0) cx = 0; if (cx >= w) cx = w - 1;
    if (cy < 0) cy = 0; if (cy >= h) cy = h - 1;

    WireEvent e{};
    e.kind = 'M';
    e.a    = (unsigned short)(buttons | WIRE_M_ABS);
    e.b    = (unsigned short)wheel;
    e.dx   = (short)(unsigned short)((cx * 65535) / (w > 1 ? w - 1 : 1));
    e.dy   = (short)(unsigned short)((cy * 65535) / (h > 1 ? h - 1 : 1));
    wire_send(e);
}

static void send_key(unsigned short scan, bool up, bool ext)
{
    WireEvent e{};
    e.kind = 'K';
    e.a    = scan;
    e.b    = (unsigned short)((up ? I_KEY_UP : 0) | (ext ? I_KEY_E0 : 0));
    wire_send(e);
}

/* Release every modifier in the seat.
 *
 * MUST be called whenever this window loses focus. A modifier's DOWN is
 * forwarded while we have focus, but if focus moves before the UP -- Alt-Tab,
 * clicking away, the window being raised over us -- that UP is delivered to
 * whoever has focus now, never to us, and never to the seat. The seat is then
 * left holding Ctrl (or Alt, or Shift) permanently: every subsequent keystroke
 * becomes a shortcut and typing appears completely broken.
 *
 * seatB_agent clears modifiers on every reconnect for exactly this reason; the
 * injector needs the same discipline. Sending an UP for a key that is already up
 * is harmless. */
static void release_all_modifiers(void)
{
    struct { unsigned short scan; bool ext; } mods[] = {
        { 0x1D, false }, /* LCtrl  */  { 0x1D, true  }, /* RCtrl  */
        { 0x2A, false }, /* LShift */  { 0x36, false }, /* RShift */
        { 0x38, false }, /* LAlt   */  { 0x38, true  }, /* RAlt   */
        { 0x5B, true  }, /* LWin   */  { 0x5C, true  }, /* RWin   */
    };
    for (auto& m : mods) send_key(m.scan, true, m.ext);
}

/* Borderless fullscreen toggle for the view window.
 *
 * "Maximized" still leaves a title bar and the taskbar. Borderless means the
 * seat's frame fills the whole monitor -- and because this is our own window
 * with the frame simply removed, rather than an exclusive-mode fullscreen, it
 * still Alt-Tabs normally. mstsc's fullscreen traps input; this does not.
 *
 * Remembers the previous placement so the toggle is reversible. */
static bool       g_viewFull = false;
static WINDOWPLACEMENT g_viewPrev = { sizeof(WINDOWPLACEMENT) };

static void toggle_view_fullscreen(HWND h)
{
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (!g_viewFull) {
        GetWindowPlacement(h, &g_viewPrev);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtrW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right  - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_viewFull = true;
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_viewPrev);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_viewFull = false;
    }
}

/* Window proc for the VIEW window only -- the panel window keeps the plain one,
 * since it is click-through and must never take input. */
static LRESULT CALLBACK view_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_MOUSEMOVE:
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0, 0);
        return 0;
    case WM_LBUTTONDOWN: SetCapture(h);
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), I_M_LEFTDOWN, 0); return 0;
    case WM_LBUTTONUP:   ReleaseCapture();
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), I_M_LEFTUP, 0); return 0;
    case WM_RBUTTONDOWN:
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), I_M_RIGHTDOWN, 0); return 0;
    case WM_RBUTTONUP:
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), I_M_RIGHTUP, 0); return 0;
    case WM_MBUTTONDOWN:
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), I_M_MIDDLEDOWN, 0); return 0;
    case WM_MBUTTONUP:
        send_mouse_abs(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), I_M_MIDDLEUP, 0); return 0;

    case WM_MOUSEWHEEL: {
        /* Wheel arrives in screen coordinates; the agent wants client. */
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        send_mouse_abs(h, p.x, p.y, I_M_WHEEL, (short)GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }

    /* F11 toggles borderless fullscreen locally -- NOT forwarded to the seat,
     * since it is our own view control rather than something the seat should
     * see. Everything else goes through. */
    case WM_KEYDOWN:
        if (wp == VK_F11) { toggle_view_fullscreen(h); return 0; }
        [[fallthrough]];

    /* Scan codes, not virtual keys: the agent injects with KEYEVENTF_SCANCODE,
     * so the seat's own keyboard layout applies rather than ours. */
    case WM_SYSKEYDOWN:
        send_key((unsigned short)((lp >> 16) & 0xFF), false, (lp & (1 << 24)) != 0);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        send_key((unsigned short)((lp >> 16) & 0xFF), true,  (lp & (1 << 24)) != 0);
        return 0;

    /* Focus left us: whatever is held down will never get its keyup, so let go
     * of everything in the seat now. */
    case WM_KILLFOCUS:
        release_all_modifiers();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) release_all_modifiers();
        return 0;

    case WM_CLOSE:
        release_all_modifiers();   /* don't leave the seat holding keys */
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND make_view_window(int w, int h)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc   = view_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hydra_mirror_view";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    /* No background brush: GetStockObject lives in gdi32, which mirror does not
     * otherwise link, and the whole client area is covered by the presented
     * frame on the first Present anyway. */
    RegisterClassW(&wc);

    /* Size the CLIENT area to the requested dimensions, so "1920x1080" means the
     * picture is that size rather than the frame. */
    RECT r{ 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Hydra - seat view", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    /* Open BORDERLESS FULLSCREEN, not merely maximized: maximized still leaves a
     * title bar and the taskbar. This is our own window with the frame removed,
     * so unlike mstsc's fullscreen it does not trap input and Alt-Tab works
     * normally. F11 toggles back. */
    ShowWindow(hwnd, SW_SHOW);
    toggle_view_fullscreen(hwnd);
    SetForegroundWindow(hwnd);
    return hwnd;
}

static HWND make_window(const RECT& rc)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hydra_mirror";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

/* --------------------------------------------------------------------------- *
 * Metadata channel (consumer side of hydra_ipc.h).
 * --------------------------------------------------------------------------- */
struct Meta
{
    HANDLE        map = nullptr;
    HydraSeatMeta* p  = nullptr;

    bool open(const char* seat)
    {
        wchar_t name[128]; hydra_meta_name(name, 128, seat);
        map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!map) return false;
        p = (HydraSeatMeta*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                          sizeof(HydraSeatMeta));
        return p != nullptr;
    }
    void close()
    {
        if (p)  { UnmapViewOfFile(p); p = nullptr; }
        if (map){ CloseHandle(map);   map = nullptr; }
    }
    ~Meta() { close(); }
};

/* --------------------------------------------------------------------------- *
 * D3D11 device on a SPECIFIC adapter LUID (HYDRA-TODO(luid)) + flip-model
 * swapchain on the window.
 * --------------------------------------------------------------------------- */
struct Gfx
{
    ComPtr<ID3D11Device>           dev;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain1>        swap;
    UINT w{}, h{};
    HWND wnd{};

    bool init(HWND hwnd, const RECT& rc, LUID luid)
    {
        wnd = hwnd;
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;

        ComPtr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        /* Match iddseat's LUID so the shared open is zero-copy. Fall back to the
         * default adapter only if that LUID is gone. */
        ComPtr<IDXGIAdapter1> adapter;
        bool haveAdapter = false;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == luid.LowPart &&
                d.AdapterLuid.HighPart == luid.HighPart) { haveAdapter = true; break; }
        }

        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(
            haveAdapter ? adapter.Get() : nullptr,
            haveAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if (FAILED(hr)) return false;

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width  = w;
        sc.Height = h;
        sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2;
        sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;   /* flip model = low latency */
        sc.Scaling     = DXGI_SCALING_STRETCH;

        hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sc, nullptr, nullptr, &swap);
        return SUCCEEDED(hr);
    }
};

/* --------------------------------------------------------------------------- *
 * Shared surface (consumer side of hydra_ipc.h). Opened by NAME — no handle
 * duplication across the session boundary. Reopened when the producer bumps its
 * generation (swapchain reassigned) or format/size changes.
 * --------------------------------------------------------------------------- */
struct SharedSurface
{
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<IDXGIKeyedMutex> mutex;
    UINT w{}, h{};
    DXGI_FORMAT fmt{};

    bool open(Gfx& g, const char* seat)
    {
        tex.Reset(); mutex.Reset();
        ComPtr<ID3D11Device1> dev1;
        if (FAILED(g.dev.As(&dev1))) return false;

        wchar_t name[128]; hydra_surface_name(name, 128, seat);
        /* Every failure below used to return false silently, so a mirror that
         * could never open the surface looked identical to one that simply had
         * nothing to show. Report the actual HRESULT. */
        /* MUST request READ|WRITE, not READ alone.
         *
         * The surface carries a keyed mutex, and AcquireSync/ReleaseSync MUTATE
         * that mutex -- so a read-only open is rejected outright with
         * E_INVALIDARG (0x80070057), which is what silently kept this mirror
         * from ever presenting the captured desktop. The access flags must also
         * match what the producer passed to CreateSharedHandle, and
         * session_capture creates it READ|WRITE. "Read-only" is intuitive for a
         * consumer and wrong here. */
        HRESULT ohr = dev1->OpenSharedResourceByName(name,
                                                     DXGI_SHARED_RESOURCE_READ |
                                                     DXGI_SHARED_RESOURCE_WRITE,
                                                     IID_PPV_ARGS(&tex));
        if (FAILED(ohr)) {
            static HRESULT lastReported = S_OK;
            if (ohr != lastReported) {
                lastReported = ohr;
                fwprintf(stderr, L"[mirror] OpenSharedResourceByName(%ls) failed hr=0x%08lX\n",
                         name, (unsigned long)ohr);
                fflush(stderr);
            }
            return false;
        }
        fwprintf(stderr, L"[mirror] opened shared surface %ls\n", name);
        fflush(stderr);
        if (FAILED(tex.As(&mutex))) {
            fwprintf(stderr, L"[mirror] surface has no keyed mutex\n"); fflush(stderr);
            return false;
        }

        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        w = d.Width; h = d.Height; fmt = d.Format;
        return true;
    }
    void close() { mutex.Reset(); tex.Reset(); w = h = 0; }
};

/* Copy the shared surface into the backbuffer under key 0 and present. If the
 * source and panel differ in size, this uses a stretch via a fullscreen draw;
 * for the common "virtual mode == panel" case it's a straight CopyResource. */
/* Consumes frames from the shared-memory pixel transport. Replaces the shared
 * D3D11 texture, which cannot cross the session boundary now that the producer
 * lives inside the seat's RDP session (see hydra_ipc.h). */
struct PixelReader {
    HANDLE                  map = nullptr;
    const HydraSeatPixels*  hdr = nullptr;
    const BYTE*             px  = nullptr;
    ComPtr<ID3D11Texture2D> tex;      /* DYNAMIC, uploaded to each frame */
    UINT                    w = 0, h = 0;
    uint64_t                lastSeq = 0;

    /* Drop the current mapping so the next open() attaches to a NEW section. */
    void reopen() {
        if (hdr) { UnmapViewOfFile((void*)hdr); hdr = nullptr; px = nullptr; }
        if (map) { CloseHandle(map); map = nullptr; }
        lastSeq = 0;
    }

    bool open(const char* seat) {
        /* NOT just "already open, done".
         *
         * When the Hydra service restarts, hydrad creates a BRAND NEW section
         * with the same name. A mirror holding the old handle keeps reading a
         * section nobody writes to any more -- it never errors, never notices,
         * and sits frozen at a few MB of working set forever. That is what made
         * start-up order appear to matter: it was not the order, it was a stale
         * mapping that could only be cleared by restarting mirror.
         *
         * The caller detects staleness (no new frames for a while) and calls
         * reopen(); this then attaches to whatever is there now. */
        if (hdr) return true;
        wchar_t name[128]; hydra_pixels_name(name, 128, seat);
        map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!map) return false;
        const void* v = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        if (!v) { CloseHandle(map); map = nullptr; return false; }
        hdr = (const HydraSeatPixels*)v;
        px  = (const BYTE*)v + sizeof(HydraSeatPixels);
        fwprintf(stderr, L"[mirror] pixel transport opened (%ls)\n", name);
        fflush(stderr);
        return true;
    }

    bool ensure_tex(ID3D11Device* dev, UINT nw, UINT nh) {
        if (tex && w == nw && h == nh) return true;
        tex.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = nw; d.Height = nh; d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) { tex.Reset(); return false; }
        w = nw; h = nh; return true;
    }

    /* Seqlock read: snapshot must be bracketed by the same EVEN sequence value,
     * otherwise the producer was mid-write and we retry next frame rather than
     * present a torn image. */
    bool upload(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        if (!hdr) return false;
        uint64_t s1 = hdr->seq;
        if (s1 & 1ull) return false;
        if (s1 == lastSeq) return tex != nullptr;      /* nothing new */
        UINT nw = hdr->width, nh = hdr->height;
        if (!nw || !nh || nw > HYDRA_PIX_MAX_W || nh > HYDRA_PIX_MAX_H) return false;
        if (!ensure_tex(dev, nw, nh)) return false;

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        const BYTE* srow = px;
        BYTE* drow = (BYTE*)m.pData;
        for (UINT y = 0; y < nh; ++y) {
            memcpy(drow, srow, (size_t)nw * 4);
            srow += (size_t)nw * 4;
            drow += m.RowPitch;
        }
        ctx->Unmap(tex.Get(), 0);

        uint64_t s2 = hdr->seq;
        if (s2 != s1) return false;                    /* torn; skip this one */
        lastSeq = s1;
        return true;
    }
};

static PixelReader g_pix;

/* Set when the GPU device is lost; the main loop rebuilds Gfx from scratch.
 * Nothing created from a removed device can be reused, so partial recovery is
 * not an option. */
static bool g_deviceLost = false;

/* Present from the shared-memory transport. */
static bool present_pixels(Gfx& g, const char* seat)
{
    static DWORD lastLog = 0;
    static uint64_t shown = 0;

    if (!g_pix.open(seat)) return false;               /* producer not up yet */
    if (!g_pix.upload(g.dev.Get(), g.ctx.Get())) {
        if (!g_pix.tex) return false;                  /* nothing to show yet */
    }

    /* Match the backbuffer to the frame so DXGI_SCALING_STRETCH scales it to the
     * panel, instead of the old clip-to-overlap behaviour. */
    if (g_pix.w && (g_pix.w != g.w || g_pix.h != g.h)) {
        if (SUCCEEDED(g.swap->ResizeBuffers(2, g_pix.w, g_pix.h,
                                            DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            fwprintf(stderr, L"[mirror] backbuffer resized %ux%u -> %ux%u (now matches the frame)\n",
                     g.w, g.h, g_pix.w, g_pix.h);
            fflush(stderr);
            g.w = g_pix.w; g.h = g_pix.h;
        }
    }

    /* RE-ASSERT TOPMOST EVERY FRAME.
     *
     * WS_EX_TOPMOST at creation is not enough. This process is launched by the
     * Hydra service into the console session, and a service-launched process has
     * no foreground-activation rights -- so its window is created behind
     * whatever already owns the top band (a fullscreen mstsc, for instance) and
     * can never raise itself. The symptom is brutal to diagnose: mirror reports
     * a perfectly healthy present loop, frames counting up, and nothing visible
     * on the panel -- while the SAME binary run by hand from an interactive shell
     * works, because that one DOES get activation rights.
     *
     * cursor_overlay hit this exact wall and solved it the same way. Cheap call,
     * and it self-corrects whenever anything else steals the top band. */
    SetWindowPos(g.wnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g.ctx->CopyResource(back.Get(), g_pix.tex.Get());
        ++shown;
    }

    /* CHECK WHAT PRESENT RETURNS.
     *
     * Ignoring it meant a GPU device reset went completely unnoticed: the panel
     * went black while this process cheerfully carried on incrementing
     * "presented=" every frame, because every step still "succeeded" locally.
     * The giveaway in the log was an unrelated-looking 0x88760870 appearing
     * mid-run -- a DXGI device-removed error.
     *
     * A device reset happens for ordinary reasons: a display driver update, a
     * resolution or mode change, waking from hibernate, or the GPU TDR-ing. The
     * D3D device and everything created from it are permanently dead afterwards
     * and cannot be revived -- only rebuilt. */
    HRESULT ph = g.swap->Present(1, 0);
    if (ph == DXGI_ERROR_DEVICE_REMOVED || ph == DXGI_ERROR_DEVICE_RESET ||
        ph == DXGI_ERROR_DEVICE_HUNG    || ph == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
    {
        HRESULT reason = g.dev ? g.dev->GetDeviceRemovedReason() : ph;
        fwprintf(stderr, L"[mirror] device lost (Present hr=0x%08lX, reason=0x%08lX)"
                         L" -- rebuilding\n",
                 (unsigned long)ph, (unsigned long)reason);
        fflush(stderr);
        g_deviceLost = true;      /* main loop tears down and re-inits Gfx */
        return false;
    }
    else if (FAILED(ph)) {
        static HRESULT lastPh = S_OK;
        if (ph != lastPh) {
            lastPh = ph;
            fwprintf(stderr, L"[mirror] Present failed hr=0x%08lX\n", (unsigned long)ph);
            fflush(stderr);
        }
    }

    DWORD nowMs = GetTickCount();
    /* STALL DETECTOR.
     *
     * A frozen panel is invisible from inside: every component is alive, frames
     * are "presented", and nothing errors -- the picture simply stops changing.
     * The give-away is the producer's sequence number standing still. Say so
     * loudly, and name the usual cause, rather than leaving it silent. */
    {
        static uint64_t stallSeq = 0;
        static DWORD    stallSince = 0;
        static bool     reported = false;
        if (g_pix.lastSeq != stallSeq) {
            stallSeq = g_pix.lastSeq; stallSince = nowMs; reported = false;
        } else if (stallSince && (nowMs - stallSince) > 10000) {
            /* Ten seconds with no new frame: the producer may simply be idle, or
             * our mapping may be stale because the service was restarted and the
             * section replaced. Re-opening costs almost nothing and is the only
             * way to tell the two apart, so just do it -- if the section is the
             * same one, we attach straight back to it.
             *
             * This is what makes start-up ORDER stop mattering: a mirror started
             * before the producer, or left behind by a service restart, now finds
             * its way back on its own instead of sitting dead until someone
             * notices and restarts it by hand. */
            static DWORD lastReopen = 0;
            if (nowMs - lastReopen > 10000) {
                lastReopen = nowMs;
                g_pix.reopen();
                fwprintf(stderr, L"[mirror] no frames for 10s -- re-attaching to the "
                                 L"pixel transport in case the service restarted\n");
                fflush(stderr);
            }
        }
        if (!reported && stallSince && (nowMs - stallSince) > 120000) {
            /* 2 MINUTES, not 20 seconds.
             *
             * Desktop Duplication publishes only on CHANGE, so an idle desktop
             * legitimately produces no frames. At 20s this fired constantly on a
             * perfectly healthy system -- every time nobody was touching the seat
             * -- and those false alarms sent us chasing window sizes for hours.
             * Two minutes of complete stillness is unusual enough to be worth
             * saying, and the wording no longer asserts a cause it cannot know. */
            reported = true;
            fwprintf(stderr, L"[mirror] no new frames for 2 min (seq stuck at %llu). "
                             L"Either the seat's desktop is genuinely idle -- which is "
                             L"normal -- or it has stopped being composed. Check with: "
                             L"mirror.exe <seat> --probe 15 <port>\n",
                     (unsigned long long)stallSeq);
            fflush(stderr);
        }
    }

    if (nowMs - lastLog >= 300000) {   /* 5 min heartbeat, not 4 s */
        lastLog = nowMs;
        fwprintf(stderr, L"[mirror] pixels: %ux%u seq=%llu presented=%llu\n",
                 g_pix.w, g_pix.h, (unsigned long long)g_pix.lastSeq,
                 (unsigned long long)shown);
        fflush(stderr);
    }
    return true;
}

static void present_frame(Gfx& g, SharedSurface& s)
{
    /* Diagnostics FIRST. An earlier version of this report sat after AcquireSync
     * and never printed -- which was ambiguous between "mirror never presents"
     * and "mirror can't get the keyed mutex". Those need telling apart. */
    HRESULT acq = s.mutex->AcquireSync(HYDRA_MUTEX_KEY, HYDRA_ACQUIRE_TIMEOUT_MS);
    if (acq != S_OK) return;

    /* SCALE, don't clip.
     *
     * The swapchain is created at PANEL size, so when the captured desktop is a
     * different size (1920x1080 source onto a 6720x3780 panel) the old code fell
     * into a "copy the overlapping region" path and dropped the image into the
     * top-left corner at 1:1 -- the rest of the panel black. The comment above
     * it promised a stretch that was never implemented.
     *
     * The swapchain is already DXGI_SCALING_STRETCH, so the fix is simply to
     * make the BACK BUFFER match the source: DXGI then stretches it to fill the
     * window for free, in the display hardware, with no shader needed. Done once
     * per size change, not per frame. */
    if (s.w && s.h && (s.w != g.w || s.h != g.h)) {
        HRESULT rb = g.swap->ResizeBuffers(2, s.w, s.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (SUCCEEDED(rb)) {
            fwprintf(stderr, L"[mirror] backbuffer resized %ux%u -> %ux%u (DXGI stretches to panel)\n",
                     g.w, g.h, s.w, s.h);
            fflush(stderr);
            g.w = s.w; g.h = s.h;
        } else {
            fwprintf(stderr, L"[mirror] ResizeBuffers failed hr=0x%08lX\n", (unsigned long)rb);
            fflush(stderr);
        }
    }

    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back))))
    {
        if (s.w == g.w && s.h == g.h)
        {
            g.ctx->CopyResource(back.Get(), s.tex.Get());
        }
        else
        {
            /* Size mismatch: copy the overlapping region (letterbox). A sampler
             * blit would scale; for a mirrored head we keep it exact and clip. */
            D3D11_BOX box{ 0, 0, 0,
                           (s.w < g.w ? s.w : g.w), (s.h < g.h ? s.h : g.h), 1 };
            g.ctx->CopySubresourceRegion(back.Get(), 0, 0, 0, 0, s.tex.Get(), 0, &box);
        }
    }

    s.mutex->ReleaseSync(HYDRA_MUTEX_KEY);
    g.swap->Present(1, 0);   /* vsync-paced */
}

static BOOL CALLBACK list_cb(HMONITOR h, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, (MONITORINFO*)&mi))
        fwprintf(stdout, L"  %-14s %ldx%ld at (%ld,%ld)%s\n",
                 mi.szDevice,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 (mi.dwFlags & MONITORINFOF_PRIMARY) ? L"  (primary)" : L"");
    return TRUE;
}

/* --------------------------------------------------------------------------- */
int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        /* Bare invocation doubles as the monitor-discovery tool: print every
         * attached display's stable device name for seats.toml. */
        fwprintf(stdout, L"attached monitors (use the \\\\.\\DISPLAYn name in seats.toml):\n");
        EnumDisplayMonitors(nullptr, nullptr, list_cb, 0);
        fwprintf(stdout, L"\nusage: mirror.exe <seat-name> <\\\\.\\DISPLAYn>\n");
        return 2;                                  /* config error (respawn contract) */
    }
    const wchar_t* seatW = argv[1];
    const wchar_t* dev   = argv[2];
    char seat[64] = {};
    for (int i = 0; seatW[i] && i < 63; ++i) seat[i] = (char)seatW[i];

    /* Windowed view mode:  mirror.exe B --window [WxH]
     * Shows the seat in a normal resizable window on this screen instead of
     * taking over a panel. Resize it freely -- the seat's 1920x1080 frame is
     * scaled to fit, which is the thing mstsc refuses to do. */
    /* PROBE MODE:  mirror.exe <seat> --probe [seconds]
     *
     * Opens the pixel transport, samples the producer's sequence number, and
     * reports how many frames arrived. Nothing is displayed.
     *
     * Exists because "is the panel frozen?" was being judged by staring at a
     * monitor for minutes at a time, which is slow and unreliable -- and the
     * question came up for every candidate client-window size. This turns it
     * into a number.
     *
     * NOTE: the seat's desktop must have something CHANGING on it (a clock, a
     * blinking caret, a video). Desktop Duplication publishes on change, so a
     * genuinely idle desktop legitimately produces almost no frames and would
     * read as frozen. */
    if (_wcsicmp(dev, L"--probe") == 0) {
        int secs = 10;
        int port = 0;
        for (int i = 3; i < argc; ++i) {
            int v = _wtoi(argv[i]);
            if (v > 0 && v < 3600 && secs == 10 && v < 1024) secs = v;
            else if (v >= 1024 && v < 65536) port = v;
        }

        if (!g_pix.open(seat)) {
            fwprintf(stderr, L"[probe] no pixel transport for seat %s -- is capture running?\n", seatW);
            return 2;
        }
        /* GENERATE ACTIVITY, or the measurement is meaningless.
         *
         * Desktop Duplication publishes ONLY on change. A probe run while the
         * seat's desktop happens to be idle therefore reports zero frames and
         * looks exactly like a hard freeze -- which is precisely the false alarm
         * this tool was built to eliminate. The giveaway was seq advancing
         * BETWEEN probe runs while each run measured nothing.
         *
         * Given the router's inject port, nudge the seat's cursor once a second
         * so there is guaranteed to be something to capture. Without a port we
         * can only warn. */
        if (port) {
            WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);
            /* +1000: the router's INJECT listener, not the agent port.
             *
             * The agent port is where seatB_agent connects TO the router, and
             * that accept loop is "newest connection wins" -- connecting there
             * DISPLACES the real agent and kills the seat's input. The view-mode
             * path already added this thousand; the probe path did not, so a
             * diagnostic tool was breaking the thing it was measuring. */
            g_inPort = port + 1000;
            input_connect();
            if (g_inSock == INVALID_SOCKET)
                fwprintf(stderr, L"[probe] could not reach the inject port %d -- "
                                 L"measuring without generated activity\n", port);
        } else {
            fwprintf(stderr, L"[probe] no inject port given: this measures nothing if the "
                             L"seat's desktop is IDLE. Pass the seat port (e.g. 56789) to "
                             L"have the probe move the cursor itself.\n");
        }

        uint64_t a = g_pix.hdr->seq;
        fwprintf(stderr, L"[probe] seat %s: sampling %d s (seq starts at %llu)\n",
                 seatW, secs, (unsigned long long)a);
        fflush(stderr);

        for (int t = 0; t < secs; ++t) {
            if (g_inSock != INVALID_SOCKET) {
                /* Two positions, alternating -- a move to the SAME place would
                 * change no pixels and defeat the point. */
                WireEvent e{};
                e.kind = 'M';
                e.a    = WIRE_M_ABS;
                e.dx   = (short)(unsigned short)((t & 1) ? 20000 : 40000);
                e.dy   = (short)(unsigned short)((t & 1) ? 20000 : 40000);
                wire_send(e);
            }
            Sleep(1000);
        }

        uint64_t b = g_pix.hdr->seq;
        /* seq advances TWICE per frame -- odd entering the write, even leaving
         * it, which is what makes the seqlock work. Reporting the raw delta as a
         * frame count therefore doubled every figure this tool has ever printed:
         * a 60fps throttle read as 134fps, which looked like the throttle was
         * not working when it was exactly right. */
        uint64_t frames = (b - a) / 2;
        double fps = (double)frames / (double)secs;
        fwprintf(stderr, L"[probe] seat %s: %llu frames in %d s (%.1f fps) -- %ls\n",
                 seatW, (unsigned long long)frames, secs, fps,
                 frames < 2 ? (port ? L"FROZEN" : L"no frames (idle desktop, or frozen)")
                             : L"alive");
        fflush(stderr);
        return ((b - a) < 3) ? 1 : 0;    /* exit code: 0 alive, 1 frozen */
    }

    bool viewMode = (_wcsicmp(dev, L"--window") == 0);
    int  viewW = 1280, viewH = 720;
    for (int i = 3; viewMode && i < argc; ++i) {
        int a = 0, b = 0, port = 0;
        if (swscanf(argv[i], L"%dx%d", &a, &b) == 2 && a > 160 && b > 120) {
            viewW = a; viewH = b;
        } else if (swscanf(argv[i], L"%d", &port) == 1 && port > 0 && port < 65536) {
            /* Seat port from seats.toml. We connect to port+1000, the router's
             * INJECT listener -- NOT the agent port.
             *
             * The agent port is where seatB_agent connects TO the router, and
             * that accept loop is "newest connection wins": an injector arriving
             * there displaces the real agent and the seat loses input entirely.
             * That was measured the hard way. The inject listener is a separate
             * socket that reads events and forwards them into the seat. */
            g_inPort = port + 1000;
        }
    }
    if (viewMode && g_inPort) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    /* Per-Monitor-V2, not the Vista-era SetProcessDPIAware(). The old call sets

     * SYSTEM awareness, so on a mixed-DPI box every monitor rect comes back

     * virtualised against the PRIMARY monitor's scale -- DISPLAY2 reported

     * 6720x3780 for a 1920x1080 panel. V2 gives true physical pixels.

     * Falls back to the old call on anything before 1703. */

    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))

        SetProcessDPIAware();
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    /* WAIT for the panel rather than exiting if it isn't there yet.
     *
     * mirror starts at LOGON, and external displays are frequently not
     * enumerated that early -- so \\.\DISPLAYn simply doesn't exist for the
     * first few seconds. The old code printed "monitor not found" and exited
     * with code 2, which is why the panel worked perfectly when mirror was
     * started by hand and was dead after every reboot: by the time anyone
     * looked, the process was long gone.
     *
     * Poll indefinitely instead. There is nothing useful to do without the
     * panel, and a monitor can legitimately appear (or come back) at any time --
     * a cable replug, a dock, a resolution change. */
    MonMatch mm{};
    if (!viewMode) {
        DWORD waited = 0;
        for (;;) {
            mm = find_monitor(dev);
            if (mm.found) break;
            if (waited == 0 || waited % 30000 == 0)
                fwprintf(stderr, L"[mirror %s] monitor %s not present yet; waiting\n", seatW, dev);
            fflush(stderr);
            Sleep(1000);
            waited += 1000;
        }
        if (waited)
            fwprintf(stderr, L"[mirror %s] monitor %s appeared after %lums\n", seatW, dev, waited);
        fflush(stderr);
    }
    HWND hwnd;
    if (viewMode) {
        hwnd = make_view_window(viewW, viewH);
        GetClientRect(hwnd, &mm.rc);
        mm.found = true;
        fwprintf(stderr, L"[mirror %s] windowed view %dx%d (resize freely; the seat's "
                         L"frame is scaled to fit)\n", seatW, viewW, viewH);
        fflush(stderr);
    } else {
        hwnd = make_window(mm.rc);
    }

    /* Wait for the producer's metadata to be ready (iddseat may start after us).
     * Read the LUID from it, then build our device on that GPU. */
    /* Wait for the producer's metadata, but DON'T BLOCK FOREVER on it.
     *
     * mirror now starts at logon -- before teacher's session exists and before
     * the Hydra service has created the shared sections. The old unbounded waits
     * meant it sat here indefinitely and never even built its device, so the
     * panel stayed dark until someone restarted it by hand.
     *
     * The metadata is only used to pick the adapter LUID, and the pixel transport
     * uploads from the CPU anyway, so adapter matching is a nicety rather than a
     * requirement. Give it a few seconds; if it isn't there, carry on with the
     * default adapter. The main loop picks up the transport whenever it appears. */
    Meta meta;
    LUID luid{};
    {
        const DWORD deadline = GetTickCount() + 5000;
        while (!meta.open(seat) && GetTickCount() < deadline)
        {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { if (m.message==WM_QUIT) return 0; }
            Sleep(200);
        }
        while (meta.p && (!meta.p->ready || meta.p->version != HYDRA_IPC_VERSION)
               && GetTickCount() < deadline)
        {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { if (m.message==WM_QUIT) return 0; }
            Sleep(50);
        }
        if (meta.p && meta.p->ready) {
            luid.LowPart = meta.p->luidLow; luid.HighPart = meta.p->luidHigh;
        } else {
            fwprintf(stderr, L"[mirror] producer not up yet; using default adapter "
                             L"and waiting for the pixel transport\n");
            fflush(stderr);
        }
    }

    Gfx g{};
    if (!g.init(hwnd, mm.rc, luid))
    {
        fwprintf(stderr, L"[mirror %s] D3D init failed\n", seatW);
        return 1;                                  /* fault (respawn restarts) */
    }

    SharedSurface s{};
    UINT lastGen = 0;
    bool haveSurface = false;

    fwprintf(stderr, L"[mirror %s] presenting on %s (%ux%u), matching LUID %08lx:%08lx\n",
             seatW, dev, g.w, g.h, (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);

    MSG msg{};
    for (;;)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        /* (Re)open the shared surface on first use and whenever the producer
         * signals a new generation (its swapchain was reassigned = session
         * rebind, resolution change, driver reload). */
        UINT gen = meta.p ? meta.p->generation : 0;
        bool ready = meta.p && meta.p->ready;
        if (ready && (!haveSurface || gen != lastGen))
        {
            s.close();
            haveSurface = s.open(g, seat);
            lastGen = gen;
            if (!haveSurface) Sleep(50);
        }
        else if (!ready && haveSurface)
        {
            s.close(); haveSurface = false;         /* producer retired; wait */
        }

        /* Heartbeat so a mirror that is alive but idle can be told apart from
         * one that is stuck: shows whether the producer says the surface is
         * ready and whether we managed to open it. */
        {
            static DWORD lastState = 0;
            DWORD nowMs = GetTickCount();
            if (false) {
                lastState = nowMs;
                fwprintf(stderr, L"[mirror] state: ready=%d haveSurface=%d gen=%u\n",
                         ready ? 1 : 0, haveSurface ? 1 : 0, (unsigned)gen);
                fflush(stderr);
            }
        }

        /* A lost GPU device means every D3D object we hold is dead. Rebuild the
         * whole graphics stack -- swapchain, device, the lot -- and let the
         * pixel reader re-upload into fresh textures. */
        if (g_deviceLost) {
            g_deviceLost = false;
            fwprintf(stderr, L"[mirror] re-initialising graphics after device loss\n");
            fflush(stderr);
            g_pix.tex.Reset();          /* belonged to the dead device */
            g_pix.w = g_pix.h = 0;
            g_pix.lastSeq = 0;          /* force a fresh upload */
            g = Gfx{};
            /* In windowed mode there is no panel to look up -- rebuild against
             * the existing window instead. */
            MonMatch mm2{};
            if (viewMode) { GetClientRect(hwnd, &mm2.rc); mm2.found = true; }
            else          { mm2 = find_monitor(dev); }
            if (mm2.found) {
                HWND hwnd2 = viewMode ? hwnd : make_window(mm2.rc);
                if (!g.init(hwnd2, mm2.rc, luid)) {
                    fwprintf(stderr, L"[mirror] re-init failed; retrying\n");
                    fflush(stderr);
                    Sleep(1000);
                    continue;
                }
                fwprintf(stderr, L"[mirror] graphics re-initialised\n");
                fflush(stderr);
            } else {
                Sleep(1000);
                continue;
            }
        }

        /* Present, and SLEEP when there is nothing to present.
         *
         * Present() blocks on vsync only when it actually runs. On the idle path
         * -- producer not started, no new frame yet -- present_pixels returns
         * immediately, so without this sleep the loop spins a core flat out. That
         * is exactly what happened when mirror started at logon before the
         * capture agent existed: hundreds of seconds of CPU, a 2.7 MB working
         * set, and nothing on the panel. Cheap mistake, expensive symptom. */
        if (!present_pixels(g, seat))
            Sleep(30);
    }
}
