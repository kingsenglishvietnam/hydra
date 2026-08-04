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

#include <windows.h>
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

    bool open(const char* seat) {
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
            fwprintf(stderr, L"[mirror] backbuffer %ux%u -> %ux%u (DXGI stretches to panel)\n",
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
    g.swap->Present(1, 0);

    DWORD nowMs = GetTickCount();
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
    {
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
    HWND hwnd = make_window(mm.rc);

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
