/* session_capture.cpp  --  capture THIS session's desktop and publish it into the
 * Hydra shared surface, so `mirror` presents it on the physical panel.
 *
 * WHY THIS EXISTS (Goal 3, "no RDP window")
 *   The console IDD (iddseat) can only show the *console* desktop. To put a
 *   *different session's* desktop on the panel without an RDP window, we flip the
 *   producer: this agent runs INSIDE the target session (hydrad launches it there
 *   the same way it launches seatB_agent), captures that session's composed
 *   desktop with the Desktop Duplication API, and writes each frame into the SAME
 *   named shared surface (Global\HydraSeat_<seat>_surf) that `mirror` already
 *   reads. mirror is unchanged -- it just presents whatever's in the surface.
 *
 *   Net effect: teacher's real session, on the physical panel, cursor INCLUDED
 *   (Desktop Duplication captures the composed desktop with the pointer drawn in),
 *   and no mstsc window anywhere. That's the RDP-display-transport, deleted.
 *
 * WHY DDA IS FINE ON SPEED
 *   AcquireNextFrame hands back a GPU texture already in VRAM -- no CPU readback.
 *   We CopyResource GPU->GPU into the shared texture. ~1-2 ms/frame; this is the
 *   same path OBS uses for 60fps capture. The old "capture is too slow" worry was
 *   about GDI BitBlt, not DDA.
 *
 * MUST RUN IN THE TARGET SESSION on an interactive desktop. Desktop Duplication
 * fails with E_ACCESSDENIED if the process isn't attached to the input desktop of
 * a session with a display. hydrad's CreateProcessAsUserW into the session handles
 * that (same as the agent). For a quick manual test, run it inside that session.
 *
 * BUILD (needs D3D11 + DXGI):
 *   cl /O2 /EHsc session_capture.cpp /link d3d11.lib dxgi.lib user32.lib
 * USAGE:
 *   session_capture.exe <seat> [outputIndex]
 *     <seat>       matches the mirror/iddseat seat name, e.g. B
 *     outputIndex  which DISPLAY to capture in this session (default 0)
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "common/hydra_ipc.h"

using Microsoft::WRL::ComPtr;

static volatile bool g_run = true;
static BOOL WINAPI on_ctrl(DWORD) { g_run = false; return TRUE; }

static void logline(const char* seat, const char* msg, HRESULT hr = S_OK) {
    if (hr == S_OK) fprintf(stderr, "[capture %s] %s\n", seat, msg);
    else            fprintf(stderr, "[capture %s] %s (hr=0x%08lX)\n", seat, msg, (unsigned long)hr);
}

/* Producer-side shared surface + metadata. Mirrors the contract in hydra_ipc.h:
 * create a named, keyed-mutex, NT-shared BGRA texture the mirror can open by name,
 * plus a shared-memory metadata block the mirror polls. */
struct SharedTarget {
    ComPtr<ID3D11Texture2D>  tex;       /* the shared texture (producer copy) */
    ComPtr<IDXGIKeyedMutex>  mutex;     /* key-0 mutual exclusion with mirror */
    HANDLE                   metaMap = nullptr;
    HydraSeatMeta*           meta = nullptr;
    UINT w = 0, h = 0;

    bool create(ID3D11Device1* dev1, const char* seat, UINT width, UINT height,
                LUID luid) {
        w = width; h = height;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        /* Named NT handle + keyed mutex = the mirror opens it by name, cross-session. */
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        HRESULT hr = dev1->CreateTexture2D(&td, nullptr, &tex);
        if (FAILED(hr)) { logline(seat, "CreateTexture2D(shared) failed", hr); return false; }

        hr = tex.As(&mutex);
        if (FAILED(hr)) { logline(seat, "keyed mutex QI failed", hr); return false; }

        /* Give the shared texture the agreed Global\ name so mirror can open it. */
        ComPtr<IDXGIResource1> res1;
        hr = tex.As(&res1);
        if (FAILED(hr)) { logline(seat, "IDXGIResource1 QI failed", hr); return false; }

        wchar_t surfName[128];
        hydra_surface_name(surfName, 128, seat);
        HANDLE shared = nullptr;
        hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                      DXGI_SHARED_RESOURCE_WRITE, surfName, &shared);
        if (FAILED(hr)) { logline(seat, "CreateSharedHandle(named) failed", hr); return false; }
        /* The name keeps it reachable; we can close our copy of the handle. */
        if (shared) CloseHandle(shared);

        /* Metadata mapping the mirror polls before/while opening the surface. */
        wchar_t metaName[128];
        hydra_meta_name(metaName, 128, seat);
        /* Prefer OPENING a mapping that hydrad pre-created for us.
         *
         * Why: this process runs as the interactive USER inside the seat's
         * session, and creating a kernel object in the Global\ namespace needs
         * SeCreateGlobalPrivilege, which a plain user token does not hold. That
         * is exactly the ERROR_ACCESS_DENIED (5) this line used to die on --
         * note it was ONLY the file mapping; the named shared texture above
         * creates fine. hydrad runs as SYSTEM in session 0, has the privilege,
         * and pre-creates this section with a permissive DACL so we can open it.
         * The Create fallback keeps standalone/elevated runs working. */
        metaMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, metaName);
        if (!metaMap)
            metaMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(HydraSeatMeta), metaName);
        if (!metaMap) { logline(seat, "CreateFileMapping(meta) failed", (HRESULT)GetLastError()); return false; }
        meta = (HydraSeatMeta*)MapViewOfFile(metaMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HydraSeatMeta));
        if (!meta) { logline(seat, "MapViewOfFile(meta) failed", (HRESULT)GetLastError()); return false; }

        meta->version   = HYDRA_IPC_VERSION;
        meta->width     = width;
        meta->height    = height;
        meta->dxgiFormat= DXGI_FORMAT_B8G8R8A8_UNORM;
        meta->luidLow   = (uint32_t)luid.LowPart;
        meta->luidHigh  = (int32_t)luid.HighPart;
        meta->frame     = 0;
        meta->generation= meta->generation + 1;  /* force mirror to (re)open */
        meta->stalled   = 0;   /* we have a display again */
        MemoryBarrier();
        meta->ready     = 1;
        return true;
    }

    /* Publish "attached but no display" so hydractl can report it. Safe to call
     * before the surface exists -- the metadata section is created by hydrad. */
    void mark_stalled(uint32_t retries)
    {
        if (meta) meta->stalled = retries;
    }

    void publish() {                 /* bump frame counter (consumer dedups) */
        if (meta) { MemoryBarrier(); meta->frame = meta->frame + 1; }
    }
    void teardown() {
        if (meta) { meta->ready = 0; UnmapViewOfFile(meta); meta = nullptr; }
        if (metaMap) { CloseHandle(metaMap); metaMap = nullptr; }
    }
};

/* Attach this thread to the session's interactive input desktop (winsta0\Default).
 * Desktop Duplication fails E_ACCESSDENIED unless the calling thread is on the
 * interactive desktop of a session with a display. When hydrad launches us via
 * CreateProcessAsUserW, the process may start on a non-interactive desktop; this
 * pins us to the right one. Safe to call even if already correct. Returns true if
 * a desktop was successfully set (or was already fine). */
static bool attach_input_desktop(const char* seat) {
    /* STEP 1: get onto the interactive window station (WinSta0). A process
     * launched by a service via CreateProcessAsUserW may start on a service/
     * non-interactive window station; OpenInputDesktop then fails. Setting the
     * process window station to WinSta0 first is required before the desktop. */
    HWINSTA ws = OpenWindowStationW(L"WinSta0", FALSE,
                     WINSTA_ACCESSCLIPBOARD | WINSTA_ACCESSGLOBALATOMS |
                     WINSTA_CREATEDESKTOP  | WINSTA_ENUMDESKTOPS |
                     WINSTA_ENUMERATE      | WINSTA_READATTRIBUTES |
                     WINSTA_READSCREEN     | WINSTA_WRITEATTRIBUTES);
    if (ws) {
        if (!SetProcessWindowStation(ws))
            logline(seat, "SetProcessWindowStation(WinSta0) failed; continuing",
                    (HRESULT)GetLastError());
        else
            logline(seat, "attached to WinSta0 window station");
        /* keep ws open for process lifetime */
    } else {
        logline(seat, "OpenWindowStation(WinSta0) failed; continuing",
                (HRESULT)GetLastError());
    }

    /* STEP 2: attach the thread to the interactive input desktop. Retry a few
     * times -- right after a session starts, the input desktop can briefly be
     * unavailable, so a couple of short retries avoids a spurious first-launch
     * E_ACCESSDENIED. */
    for (int attempt = 0; attempt < 10; ++attempt) {
        HDESK d = OpenInputDesktop(0, FALSE,
                     GENERIC_READ | GENERIC_WRITE | DESKTOP_SWITCHDESKTOP);
        if (!d) {
            d = OpenDesktopW(L"Default", 0, FALSE,
                     GENERIC_READ | GENERIC_WRITE | DESKTOP_SWITCHDESKTOP);
        }
        if (d) {
            if (SetThreadDesktop(d)) {
                logline(seat, "attached to interactive input desktop");
                /* leak HDESK for process lifetime (it's the thread's desktop) */
                return true;
            }
            /* SetThreadDesktop fails if the thread already owns user objects.
             * On the first iteration nothing should, so this is rare; if it
             * happens, close and retry after a beat. */
            CloseDesktop(d);
        }
        Sleep(200);
    }
    logline(seat, "could not attach to input desktop after retries; "
                  "DDA will likely fail E_ACCESSDENIED",
            (HRESULT)GetLastError());
    return false;
}


/* ---------------------------------------------------------------------------
 * CURSOR COMPOSITING
 *
 * Desktop Duplication deliberately EXCLUDES the mouse cursor from the captured
 * frame. It hands you the pointer shape and position as separate metadata and
 * expects you to draw it yourself. Ignoring that metadata is why capture mode
 * showed a cursor-less desktop and needed cursor_overlay.exe running inside the
 * session as a workaround.
 *
 * Because the duplicated desktop never contains a cursor, we get a clean
 * background on every frame -- so we can simply re-composite at the current
 * position each time with no need to keep a pristine copy around.
 *
 * Windows supplies three pointer formats and they are NOT interchangeable:
 *   COLOR        - straight-alpha BGRA, ordinary blend
 *   MONOCHROME   - 1bpp, DOUBLE height: top half AND-mask, bottom half XOR-mask.
 *                  AND=1,XOR=1 means INVERT the destination pixel (that's how
 *                  the classic I-beam stays visible over any background).
 *   MASKED_COLOR - BGRA where alpha 0xFF means XOR with destination, 0 means
 *                  replace.
 * Handling only COLOR would leave text-editing and busy cursors broken.
 * ------------------------------------------------------------------------- */
struct CursorState {
    std::vector<BYTE>                  shape;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO    si{};
    POINT                              pos{0,0};
    bool                               visible = false;
    bool                               haveShape = false;
    /* Why the last composite didn't draw. "drawn" counting CALLS rather than
     * actual draws hid this: the function has several early exits that all look
     * identical from the outside. */
    int  skip = -1;   /* 0=drew 1=no shape 2=zero size 3=clipped 4=staging 5=map */
    UINT lastW = 0, lastH = 0; int lastX = 0, lastY = 0;
    /* How many destination pixels the blend ACTUALLY changed, plus the alpha
     * range of the source shape. skip=0 only means we reached the end of the
     * function -- if every source pixel has alpha 0 the blend is a no-op and
     * still "succeeds", which looks identical to working code from outside. */
    UINT touched = 0, aMin = 255, aMax = 0;
};

static inline void blend_px(BYTE* d, const BYTE* s)
{
    const UINT a = s[3];
    if (a == 0) return;
    if (a == 255) { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; return; }
    d[0] = (BYTE)((s[0]*a + d[0]*(255-a)) / 255);
    d[1] = (BYTE)((s[1]*a + d[1]*(255-a)) / 255);
    d[2] = (BYTE)((s[2]*a + d[2]*(255-a)) / 255);
}

static void composite_cursor(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                             ID3D11Texture2D* dst, UINT dstW, UINT dstH,
                             CursorState& cur, ComPtr<ID3D11Texture2D>& staging,
                             UINT& stageW, UINT& stageH)
{
    cur.skip = 1;
    if (!cur.visible || !cur.haveShape || cur.shape.empty()) return;

    UINT cw = cur.si.Width;
    UINT chh = (cur.si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
             ? cur.si.Height / 2 : cur.si.Height;
    cur.skip = 2;
    if (cw == 0 || chh == 0) return;

    /* Clip to the surface, tracking how far into the cursor bitmap we start so a
     * cursor half off the edge still draws its visible part correctly. */
    int dx = cur.pos.x, dy = cur.pos.y;
    UINT offx = 0, offy = 0;
    if (dx < 0) { offx = (UINT)(-dx); dx = 0; }
    if (dy < 0) { offy = (UINT)(-dy); dy = 0; }
    cur.skip = 3;
    if (offx >= cw || offy >= chh) return;
    if ((UINT)dx >= dstW || (UINT)dy >= dstH) return;
    UINT w = cw - offx, h = chh - offy;
    if (dx + w > dstW) w = dstW - dx;
    if (dy + h > dstH) h = dstH - dy;
    if (w == 0 || h == 0) return;
    cur.lastW = w; cur.lastH = h; cur.lastX = dx; cur.lastY = dy;

    /* Staging texture for the read-modify-write. Only the cursor rect, so the
     * GPU->CPU->GPU round trip is a few KB rather than a full frame. */
    if (!staging || stageW < w || stageH < h) {
        staging.Reset();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = stageW = (w > 128 ? w : 128);
        sd.Height = stageH = (h > 128 ? h : 128);
        sd.MipLevels = sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        cur.skip = 4;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { staging.Reset(); return; }
    }

    D3D11_BOX box{}; box.left = dx; box.top = dy; box.front = 0;
    box.right = dx + w; box.bottom = dy + h; box.back = 1;
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, dst, 0, &box);

    D3D11_MAPPED_SUBRESOURCE m{};
    cur.skip = 5;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ_WRITE, 0, &m))) return;

    BYTE* base = (BYTE*)m.pData;
    const BYTE* sh = cur.shape.data();
    const UINT pitch = cur.si.Pitch;

    if (cur.si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        cur.touched = 0; cur.aMin = 255; cur.aMax = 0;
        for (UINT y = 0; y < h; ++y) {
            BYTE* drow = base + (size_t)y * m.RowPitch;
            const BYTE* srow = sh + (size_t)(y + offy) * pitch;
            for (UINT x = 0; x < w; ++x) {
                const BYTE* sp = srow + (size_t)(x + offx) * 4;
                if (sp[3] < cur.aMin) cur.aMin = sp[3];
                if (sp[3] > cur.aMax) cur.aMax = sp[3];
                if (sp[3]) ++cur.touched;
                blend_px(drow + (size_t)x * 4, sp);
            }
        }
    } else if (cur.si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        for (UINT y = 0; y < h; ++y) {
            BYTE* drow = base + (size_t)y * m.RowPitch;
            const BYTE* srow = sh + (size_t)(y + offy) * pitch;
            for (UINT x = 0; x < w; ++x) {
                const BYTE* s = srow + (size_t)(x + offx) * 4;
                BYTE* d = drow + (size_t)x * 4;
                if (s[3] == 0xFF) { d[0] ^= s[0]; d[1] ^= s[1]; d[2] ^= s[2]; }
                else              { d[0]  = s[0]; d[1]  = s[1]; d[2]  = s[2]; }
            }
        }
    } else { /* MONOCHROME */
        const BYTE* andM = sh;
        const BYTE* xorM = sh + (size_t)pitch * chh;   /* second half */
        for (UINT y = 0; y < h; ++y) {
            BYTE* drow = base + (size_t)y * m.RowPitch;
            const UINT sy = y + offy;
            for (UINT x = 0; x < w; ++x) {
                const UINT sx = x + offx;
                const UINT byteI = sy * pitch + (sx >> 3);
                const UINT bit   = 7 - (sx & 7);
                const UINT a = (andM[byteI] >> bit) & 1;
                const UINT xr = (xorM[byteI] >> bit) & 1;
                BYTE* d = drow + (size_t)x * 4;
                if (a == 0) { BYTE v = xr ? 0xFF : 0x00; d[0]=d[1]=d[2]=v; }
                else if (xr) { d[0] = (BYTE)~d[0]; d[1] = (BYTE)~d[1]; d[2] = (BYTE)~d[2]; }
                /* a==1, xr==0 -> transparent, leave the desktop pixel alone */
            }
        }
    }

    ctx->Unmap(staging.Get(), 0);

    D3D11_BOX back{}; back.left = 0; back.top = 0; back.front = 0;
    back.right = w; back.bottom = h; back.back = 1;
    ctx->CopySubresourceRegion(dst, 0, dx, dy, 0, staging.Get(), 0, &back);
    cur.skip = 0;
}

/* ---------------------------------------------------------------------------
 * Everything DDA needs, rebuilt from scratch on every (re)acquire.
 *
 * WHY A FULL REBUILD: when teacher's RDP session disconnects, Windows tears
 * down that session's display. The D3D device AND the IDXGIOutput we were
 * duplicating both become permanently stale -- so the old recovery path, which
 * just re-called DuplicateOutput() on the SAME objects, could never come back.
 * It logged 0x887A0002 (DXGI_ERROR_NOT_FOUND) forever, the process eventually
 * gave up and exited, hydrad restarted it with doubling backoff, and the panel
 * stayed dead until someone toggled the display mode by hand. Recreating the
 * factory, adapter, device and output from nothing is what actually recovers.
 * ------------------------------------------------------------------------- */
/* Publishes composited frames as PIXELS in shared memory, because a D3D11 shared
 * texture cannot cross the session boundary between this agent (in the seat's RDP
 * session) and mirror (in the console session). See hydra_ipc.h. */
struct PixelPublisher {
    HANDLE                  map = nullptr;
    HydraSeatPixels*        hdr = nullptr;
    BYTE*                   px  = nullptr;
    ComPtr<ID3D11Texture2D> staging;
    UINT                    sw = 0, sh = 0;

    bool open(const char* seat) {
        if (hdr) return true;
        wchar_t name[128]; hydra_pixels_name(name, 128, seat);
        /* hydrad creates it; we only open. Creating Global\ objects needs a
         * privilege the interactive user does not have. */
        map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!map) return false;
        void* v = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!v) { CloseHandle(map); map = nullptr; return false; }
        hdr = (HydraSeatPixels*)v;
        px  = (BYTE*)v + sizeof(HydraSeatPixels);
        return true;
    }

    bool ensure_staging(ID3D11Device* dev, UINT w, UINT h) {
        if (staging && sw == w && sh == h) return true;
        staging.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) { staging.Reset(); return false; }
        sw = w; sh = h; return true;
    }

    /* Read the composited texture back and write it into shared memory under a
     * seqlock: odd while writing, even when the snapshot is complete. */
    bool publish(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                 ID3D11Texture2D* src, UINT w, UINT h) {
        if (!hdr || w > HYDRA_PIX_MAX_W || h > HYDRA_PIX_MAX_H) return false;
        if (!ensure_staging(dev, w, h)) return false;
        ctx->CopyResource(staging.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

        hdr->seq = hdr->seq + 1;                 /* odd: write in flight */
        MemoryBarrier();
        hdr->width = w; hdr->height = h; hdr->pitch = w * 4;
        const BYTE* srow = (const BYTE*)m.pData;
        BYTE* drow = px;
        for (UINT y = 0; y < h; ++y) {
            memcpy(drow, srow, (size_t)w * 4);
            srow += m.RowPitch;
            drow += (size_t)w * 4;
        }
        MemoryBarrier();
        hdr->seq = hdr->seq + 1;                 /* even: snapshot complete */

        ctx->Unmap(staging.Get(), 0);
        return true;
    }
};

struct CaptureRig {
    ComPtr<IDXGIFactory1>          factory;
    ComPtr<IDXGIAdapter1>          adapter;
    ComPtr<ID3D11Device>           dev;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<ID3D11Device1>          dev1;
    ComPtr<IDXGIOutput1>           output1;
    ComPtr<IDXGIOutputDuplication> dupl;
    DXGI_ADAPTER_DESC1             ad{};
    UINT W = 0, H = 0;

    void reset() {
        dupl.Reset(); output1.Reset(); dev1.Reset();
        ctx.Reset();  dev.Reset();     adapter.Reset(); factory.Reset();
        W = H = 0;
    }
};

/* Build the DXGI/D3D stack and start duplicating this session's output.
 * Returns false whenever the session currently has no duplicatable display --
 * which is a NORMAL, RECOVERABLE state (RDP disconnected), not a fatal error. */
static bool acquire_rig(CaptureRig& r, const char* seat, UINT outIndex, bool verbose)
{
    r.reset();

    /* Must be on the session's interactive input desktop before DuplicateOutput
     * or it returns E_ACCESSDENIED. The desktop can change across a reconnect,
     * so re-attach every time rather than only at startup. */
    attach_input_desktop(seat);

    /* Record which session we are actually in. If the seat's RDP session is
     * reconnected, hydrad launches a NEW agent for the new session -- but an old
     * agent can linger against the dead one, attaching to a desktop that will
     * never have a display again. Logging the id makes that visible instead of
     * looking like an unexplained permanent stall. */
    if (verbose) {
        DWORD sid = 0xFFFFFFFF;
        ProcessIdToSessionId(GetCurrentProcessId(), &sid);
        char sb[96];
        snprintf(sb, sizeof(sb), "acquiring in session %lu", sid);
        logline(seat, sb);
    }

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&r.factory));
    if (FAILED(hr)) { if (verbose) logline(seat, "CreateDXGIFactory1 failed", hr); return false; }

    hr = r.factory->EnumAdapters1(0, &r.adapter);
    if (FAILED(hr)) { if (verbose) logline(seat, "EnumAdapters1(0) failed", hr); return false; }
    r.adapter->GetDesc1(&r.ad);

    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(r.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           0, nullptr, 0, D3D11_SDK_VERSION, &r.dev, &fl, &r.ctx);
    if (FAILED(hr)) { if (verbose) logline(seat, "D3D11CreateDevice failed", hr); return false; }
    if (FAILED(r.dev.As(&r.dev1))) { if (verbose) logline(seat, "ID3D11Device1 QI failed", E_NOINTERFACE); return false; }

    /* Diagnostic: how many outputs does the adapter report at all?
     *
     * "No display in session" has several possible causes and they need
     * different fixes:
     *   0 outputs        -- the session has no display object. The RDP client
     *                       stopped presenting one, or the session was
     *                       reconnected under a different display topology.
     *   >0 but ours gone -- the display list changed and outIndex now points
     *                       past the end, e.g. a resolution or monitor change.
     * Logging the count distinguishes them, and previously we could not tell. */
    UINT nOut = 0;
    for (;; ++nOut) {
        ComPtr<IDXGIOutput> probe;
        if (FAILED(r.adapter->EnumOutputs(nOut, &probe))) break;
        if (nOut > 16) break;
    }

    ComPtr<IDXGIOutput> output;
    hr = r.adapter->EnumOutputs(outIndex, &output);
    if (FAILED(hr)) {
        if (verbose) {
            char ob[160];
            snprintf(ob, sizeof(ob),
                     "adapter reports %u output(s); wanted index %u -- %s",
                     nOut, outIndex,
                     nOut == 0 ? "the SESSION HAS NO DISPLAY at all"
                               : "the display list changed under us");
            logline(seat, ob);
        }
        /* 0x887A0002 here = no display in the session. Almost always "RDP is
         * disconnected"; we simply wait for it to come back. */
        if (verbose) logline(seat, "no display in session yet (RDP disconnected?)", hr);
        return false;
    }
    if (FAILED(output.As(&r.output1))) { if (verbose) logline(seat, "IDXGIOutput1 QI failed", E_NOINTERFACE); return false; }

    hr = r.output1->DuplicateOutput(r.dev.Get(), &r.dupl);
    if (hr == E_ACCESSDENIED) {
        attach_input_desktop(seat);                     /* desktop changed under us */
        hr = r.output1->DuplicateOutput(r.dev.Get(), &r.dupl);
    }
    if (FAILED(hr)) { if (verbose) logline(seat, "DuplicateOutput failed", hr); return false; }

    DXGI_OUTDUPL_DESC dd{}; r.dupl->GetDesc(&dd);
    r.W = dd.ModeDesc.Width;
    r.H = dd.ModeDesc.Height;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: session_capture <seat> [outputIndex]\n"); return 2; }
    const char* seat = argv[1];
    UINT outIndex = (argc >= 3) ? (UINT)atoi(argv[2]) : 0;
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    CaptureRig rig;
    SharedTarget tgt;
    CursorState     cur;
    PixelPublisher  pub;
    UINT64          published = 0;
    ComPtr<ID3D11Texture2D> curStage;
    UINT stageW = 0, stageH = 0;
    /* Cursor diagnostics: prove whether DDA is actually handing us pointer data
     * before blaming the blend code. */
    UINT64 shapeUpdates = 0, posUpdates = 0, composites = 0, frames = 0, timeouts = 0;
    UINT   timeoutRun = 0;    /* consecutive timeouts; a long run means a frozen panel */
    DWORD  lastCurLog = 0;
    bool  haveTarget = false;
    int   downCycles = 0;       /* consecutive failed acquires (for quiet logging) */
    UINT64 misses = 0;

    /* Outer loop: (re)acquire, capture until something breaks, repeat forever.
     * The agent NEVER exits on display loss -- it rides through disconnect and
     * reconnect on its own, so an RDP drop no longer leaves the panel dead. */
    while (g_run) {

        if (!rig.dupl) {
            /* Log the first failure of a run, then go quiet so a long disconnect
             * doesn't fill the log with one line per second; log again on the
             * ~60s mark so there's a heartbeat. */
            /* SAY SO, REPEATEDLY.
             *
             * This used to log once and then retry in silence. A PERMANENT
             * failure therefore looked exactly like a healthy quiet log: the
             * process was alive, hydractl said "running", and the last lines
             * were the reassuring "attached to interactive input desktop" --
             * while EnumOutputs had been returning no display for hours. That
             * cost most of an evening chasing window sizes for a problem that
             * was upstream of any window.
             *
             * Now it reports every 10 s with a running count, and publishes the
             * stall to the shared metadata so hydractl can show it. */
            bool verbose = (downCycles % 10 == 0);
            if (!acquire_rig(rig, seat, outIndex, verbose)) {
                char sb[200];
                snprintf(sb, sizeof(sb),
                         "NO DISPLAY IN SESSION -- attached to the desktop, but "
                         "EnumOutputs returns nothing (retry %d, %ds). The session "
                         "has lost its duplicatable output; only a full reconnect "
                         "restores it.", downCycles, downCycles);
                if (verbose) logline(seat, sb);
                tgt.mark_stalled(downCycles);
                ++downCycles;
                Sleep(1000);
                continue;
            }
            downCycles = 0;
            tgt.mark_stalled(0);

            char buf[160];
            snprintf(buf, sizeof(buf),
                     "duplicating output %u (%ux%u); publishing to shared surface",
                     outIndex, rig.W, rig.H);
            logline(seat, buf);

            /* The shared texture belongs to the OLD device, so it must be rebuilt
             * alongside it. Tear down first so mirror sees ready=0 and reopens
             * rather than holding a handle to a dead surface. */
            if (haveTarget) { tgt.teardown(); haveTarget = false; }
            if (!tgt.create(rig.dev1.Get(), seat, rig.W, rig.H, rig.ad.AdapterLuid)) {
                logline(seat, "shared target create failed; retrying", E_FAIL);
                rig.reset();
                Sleep(1000);
                continue;
            }
            haveTarget = true;
            logline(seat, "shared surface published; entering capture loop (Ctrl+C to stop)");
        }

        /* --- Capture: acquire desktop frame -> copy into shared tex -> publish. --- */
        ComPtr<IDXGIResource> deskRes;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        HRESULT hr = rig.dupl->AcquireNextFrame(250, &fi, &deskRes);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            /* Nothing changed on the desktop. Normal for a still screen -- but a
             * LONG unbroken run of timeouts means the desktop has stopped being
             * composed at all, which is what happens when the RDP client stops
             * requesting updates (minimized, occluded, or on an inactive virtual
             * desktop). The panel then holds its last frame forever while every
             * process involved looks perfectly healthy.
             *
             * Rebuilding the duplication is cheap and sometimes recovers it; if
             * the real cause is the client, this at least says so in the log
             * instead of leaving a silent freeze. */
            ++timeouts;
            if (++timeoutRun >= 300) {          /* ~75 s at the 250 ms timeout */
                timeoutRun = 0;
                logline(seat, "no desktop updates for ~75s -- is the RDP client "
                              "minimized, covered, or on another virtual desktop? "
                              "rebuilding capture");
                rig.reset();
            }
            continue;
        }
        timeoutRun = 0;
        if (SUCCEEDED(hr)) ++frames;

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            /* Desktop switch, resolution change, lock screen, or disconnect.
             * Drop the whole rig and rebuild -- a targeted re-DuplicateOutput is
             * exactly what used to fail here. */
            logline(seat, "access lost; rebuilding capture stack");
            rig.reset();
            continue;
        }
        if (FAILED(hr)) {
            logline(seat, "AcquireNextFrame failed; rebuilding capture stack", hr);
            rig.reset();
            Sleep(200);
            continue;
        }

        /* Pointer metadata rides along with the frame. Position updates whenever
         * the mouse moves; the SHAPE only arrives when the cursor image changes,
         * so it must be cached between updates. */
        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            ++posUpdates;
            cur.visible = (fi.PointerPosition.Visible != FALSE);
            cur.pos.x = fi.PointerPosition.Position.x;
            cur.pos.y = fi.PointerPosition.Position.y;
        }
        if (fi.PointerShapeBufferSize > 0) {
            if (cur.shape.size() < fi.PointerShapeBufferSize)
                cur.shape.resize(fi.PointerShapeBufferSize);
            UINT got = 0;
            if (SUCCEEDED(rig.dupl->GetFramePointerShape(
                    (UINT)cur.shape.size(), cur.shape.data(), &got, &cur.si))) {
                cur.haveShape = true;
                ++shapeUpdates;
            }
        }

        ComPtr<ID3D11Texture2D> deskTex;
        if (SUCCEEDED(deskRes.As(&deskTex))) {
            hr = tgt.mutex->AcquireSync(HYDRA_MUTEX_KEY, HYDRA_ACQUIRE_TIMEOUT_MS);
            if (hr == S_OK) {
                rig.ctx->CopyResource(tgt.tex.Get(), deskTex.Get());
                /* Draw the cursor ON TOP. The duplicated desktop never contains
                 * one, so this is always compositing onto a clean background --
                 * no ghosting from the previous position. */
                composite_cursor(rig.dev.Get(), rig.ctx.Get(), tgt.tex.Get(),
                                 rig.W, rig.H, cur, curStage, stageW, stageH);
                if (cur.visible && cur.haveShape) ++composites;
                /* Hand the finished frame (desktop + cursor) to mirror as pixels. */
                if (pub.open(seat) &&
                    pub.publish(rig.dev.Get(), rig.ctx.Get(), tgt.tex.Get(), rig.W, rig.H))
                    ++published;

                tgt.mutex->ReleaseSync(HYDRA_MUTEX_KEY);
                tgt.publish();
            } else {
                ++misses;   /* mirror held the key; drop this frame, latest-wins */
            }
        }
        rig.dupl->ReleaseFrame();
    }

    logline(seat, "stopping");
    if (haveTarget) tgt.teardown();
    return 0;
}
