/* hydrardp.c -- Hydra's own headless RDP client.  MILESTONE 1.
 *
 * WHY BUILD A CLIENT AT ALL
 *   A stock client is a window you must keep visible. mstsc sends a Suppress
 *   Output PDU the moment its window is minimized or covered, the seat's desktop
 *   stops being composed, Desktop Duplication sees nothing, and the panel
 *   freezes. SDL-FreeRDP does not do that -- measured -- which is why it is
 *   currently the default. But it is still a window, still swaps mouse buttons,
 *   and still has to exist somewhere on screen.
 *
 *   A client we write receives the desktop, the pointer and the audio as
 *   protocol data. It needs no window at all, and it never suppresses output
 *   because we simply never send that PDU.
 *
 * WHAT IT REPLACES, once finished
 *   session_capture (DDA)        -- frames arrive as PDUs; no session boundary,
 *                                   so the cross-session texture wall that cost
 *                                   an evening does not exist here
 *   cursor compositing           -- the pointer arrives as its own PDU
 *   audio_bridge (capture half)  -- audio arrives on the same connection,
 *                                   already time-aligned with the video
 *   chime / keepalive / audio-pin-- no audio endpoint in the seat's session to
 *                                   go idle and swallow the first app's sound
 *   minify-mstsc / client-watchdog- nothing to place, nothing to keep visible
 *
 *   mirror is unchanged: it already reads the pixel ring and knows nothing about
 *   where the frames came from. That is the payoff of having built the transport
 *   first.
 *
 * =========================================================================
 * MILESTONE 1 -- THIS FILE. CONNECT, AND REPORT FRAMES. NOTHING PUBLISHED.
 * =========================================================================
 *   The risk in this project is not the code, it is the build: libfreerdp on
 *   Windows means pinning a version and tracking an API that moves between minor
 *   releases. So milestone 1 does the least that proves the chain works:
 *
 *     - link against libfreerdp
 *     - connect to 127.0.0.2 as the seat user
 *     - print the desktop size and count paints
 *
 *   If this builds and counts frames, the rest is mechanical:
 *     M2  write frames into Global\HydraSeat_<seat>_pix   (mirror displays them)
 *     M3  composite the pointer PDU into the frame
 *     M4  write audio into Global\HydraSeat_<seat>_aud
 *     M5  read the router's inject port and send input
 *
 * BUILD: see build-rdpclient.ps1. Uses MSYS2's MinGW toolchain, because that is
 * where libfreerdp already lives -- linking MSVC against an MSYS2-built library
 * is a fight with no purpose.
 *
 * THIS HAS NEVER BEEN COMPILED. There is no libfreerdp in the environment these
 * sources are written in, so the first real build is on your machine, and the
 * first attempt will almost certainly want header-path or symbol-name fixes.
 * That is expected for a first link against an unfamiliar library.
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/graphics.h>
#include <freerdp/codec/color.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/hydra_ipc.h"

typedef struct HydraContext {
    rdpContext  common;      /* MUST be first -- FreeRDP casts between them */

    char        seat[64];
    UINT64      paints;
    UINT64      published;
    UINT64      skipped;
    UINT64      copiedRows;
    /* Union of damaged rectangles since the last publish. Coalescing a paint
     * must NOT discard where it changed, or those pixels are never copied and
     * the ring keeps stale patches -- which looks like torn or smeared painting.
     * dirtyAll forces a full copy when the region is unknown. */
    UINT32      dirtyX0, dirtyY0, dirtyX1, dirtyY1;
    BOOL        dirtyAny;
    BOOL        dirtyAll;

    /* MILESTONE 3 -- the cursor.
     *
     * RDP does NOT bake the pointer into the framebuffer; it arrives as its own
     * PDU. FreeRDP's default handling draws a software pointer into the GDI
     * buffer and erases it around updates -- so sampling that buffer at
     * arbitrary moments catches the cursor drawn perhaps half the time, which is
     * exactly the "blinky cursor" symptom.
     *
     * Taking the pointer over means WE decide when it is drawn: never into
     * FreeRDP's buffer, always into our copy, once, immediately before
     * publishing. Same conclusion session_capture reached for DDA, which also
     * excludes the cursor -- different source, identical fix. */
    BYTE*       curImg;        /* current pointer as BGRA, premultiplied      */
    UINT32      curW, curH;
    UINT32      curHotX, curHotY;
    INT32       curX, curY;    /* position, in desktop pixels                 */
    BOOL        curVisible;
    BOOL        curHavePos;    /* no position yet == do not draw at (0,0)      */
    /* Where the cursor was painted LAST time. The ring is persistent and we
     * copy only the damaged rectangle, so pixels the cursor covered are never
     * restored unless that old rectangle is copied too -- which is what left
     * cursor trails smeared across the frame. */
    INT32       prevCurX, prevCurY;
    UINT32      prevCurW, prevCurH;
    BOOL        prevCurDrawn;
    CRITICAL_SECTION curLock;  /* pointer PDUs arrive off the paint thread    */
    DWORD       lastPublish;
    UINT32      lastW, lastH;
    DWORD       lastLog;

    /* Shared pixel ring -- the same transport session_capture writes and mirror
     * reads. Nothing downstream knows or cares that frames now arrive over RDP
     * instead of Desktop Duplication, which is the payoff of having built the
     * transport as a contract rather than a private arrangement. */
    HANDLE            pixMap;
    HydraSeatPixels*  pixHdr;
    BYTE*             pixData;
    BOOL              pixWarned;
} HydraContext;

/* Forward declarations: the paint callback both publishes frames and composites
 * the cursor, and sits above both. */
static BOOL hydra_open_pixels(HydraContext* h);
static void hydra_composite_pointer(HydraContext* h, UINT32 w, UINT32 ht);

/* GDI's own paint handlers, saved and CHAINED rather than replaced.
 * The graphics pipeline depends on gdi_end_paint to move surface data into the
 * framebuffer; discarding it left a null call and crashed at address 0 the
 * moment gfx attached. The bitmap path did not care, which is why this only
 * showed up with /gfx. */
static pBeginPaint g_gdiBeginPaint = NULL;
static pEndPaint   g_gdiEndPaint   = NULL;

/* Set when the pointer moves. Taking the pointer over means cursor motion no
 * longer dirties the framebuffer, so nothing triggers a paint and the cursor
 * appears to lag badly on an otherwise idle desktop. The main loop watches this
 * and republishes. */
static volatile BOOL g_curMoved = FALSE;

static volatile BOOL g_run = TRUE;

/* DISCONNECT CLEANLY, WHATEVER HAPPENS.
 *
 * This is more important than any feature. Measured four times in one evening:
 * when this client exits ABNORMALLY -- a crash, a killed process, an unhandled
 * exception -- the RDP wrapper is left holding a session it cannot clean up, and
 * every subsequent connection dies at ERRCONNECT_ACTIVATION_TIMEOUT. Only a
 * reboot clears it.
 *
 * In a classroom that is the difference between "restart the client" and
 * "restart the machine mid-lesson", so the client must tear its session down on
 * every exit path there is: Ctrl+C, console close, logoff, shutdown, and an
 * unhandled exception.
 */
static freerdp* g_inst = NULL;

static void hydra_teardown(const char* why)
{
    freerdp* inst = (freerdp*)InterlockedExchangePointer((PVOID*)&g_inst, NULL);
    if (!inst) return;                 /* someone else got there first */
    fprintf(stderr, "[hydrardp] disconnecting cleanly (%s)\n", why);
    fflush(stderr);
    freerdp_disconnect(inst);
}

static LONG WINAPI hydra_crash_filter(EXCEPTION_POINTERS* ep)
{
    /* A crash that skips the disconnect costs a REBOOT, so pay the small risk of
     * doing work in an exception filter to avoid it. */
    fprintf(stderr, "[hydrardp] FATAL: exception 0x%08lX at %p\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    hydra_teardown("crash");
    return EXCEPTION_EXECUTE_HANDLER;
}

static BOOL WINAPI on_ctrl(DWORD t)
{
    g_run = FALSE;
    /* CTRL_CLOSE/LOGOFF/SHUTDOWN give us only seconds and then kill us, so tear
     * down here rather than trusting the main loop to notice in time. */
    if (t == CTRL_CLOSE_EVENT || t == CTRL_LOGOFF_EVENT || t == CTRL_SHUTDOWN_EVENT) {
        hydra_teardown("console closing");
        Sleep(500);
    }
    return TRUE;
}

static void L(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[hydrardp] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Paint callback -- the whole point.
 *
 * FreeRDP has already decoded into the GDI primary buffer by the time this is
 * called; gdi->primary_buffer is the desktop as BGRA. Milestone 2 memcpy's that
 * into the shared pixel ring, which is all mirror needs.
 *
 * We deliberately do NOT call anything that would suppress output. The freeze
 * we spent days on is a client CHOICE, and this client never makes it.
 * ------------------------------------------------------------------------- */
static BOOL hydra_end_paint(rdpContext* context)
{
    HydraContext* h = (HydraContext*)context;
    rdpGdi* gdi = context->gdi;
    if (!gdi) return TRUE;
    /* GDI first: with the graphics pipeline this is what puts surface data into
     * primary_buffer, so copying before it runs would publish a stale frame. */
    { char gv2[8] = {0}; DWORD n2 = GetEnvironmentVariableA("HYDRA_GFX", gv2, sizeof(gv2));
      if (!(n2 > 0 && gv2[0] != 0) && g_gdiEndPaint && !g_gdiEndPaint(context)) return FALSE; }   /* BISECT 2: do not chain under gfx */

    /* GUARD THE BUFFER.
     *
     * With the graphics pipeline attached, gdi->primary_buffer is not guaranteed
     * to be present or stable the way it is on the plain bitmap path -- surfaces
     * are managed by the gfx channel and can be reallocated or absent between
     * frames. Copying from it unchecked crashed the process immediately after
     * "graphics pipeline attached", with no error of its own: the exit looked
     * like a clean stop rather than a fault. */
    if (!gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0 || gdi->stride == 0) {
        if (!h->pixWarned) {
            h->pixWarned = TRUE;
            L("no usable GDI buffer yet (buf=%p %dx%d stride=%u) -- waiting",
              (void*)gdi->primary_buffer, gdi->width, gdi->height, gdi->stride);
        }
        return TRUE;
    }

    h->paints++;
    if (gdi->width != (INT32)h->lastW || gdi->height != (INT32)h->lastH) {
        h->lastW = (UINT32)gdi->width;
        h->lastH = (UINT32)gdi->height;
        /* dstFormat, not dstBpp: rdpGdi lost the bpp field in FreeRDP 3.x and
         * exposes the pixel format instead. GetBytesPerPixel derives the depth,
         * which is what we actually care about -- the pixel ring is BGRA32 and
         * anything else means a conversion is needed. */
        L("desktop is %ux%u, format 0x%08X (%u bytes/px), stride %u",
          h->lastW, h->lastH, gdi->dstFormat,
          FreeRDPGetBytesPerPixel(gdi->dstFormat), gdi->stride);
    }

    /* Accumulate the damaged region on EVERY paint, published or not. */
    {
        HGDI_RGN inv = (gdi->primary && gdi->primary->hdc && gdi->primary->hdc->hwnd)
                     ? gdi->primary->hdc->hwnd->invalid : NULL;
        if (inv && !inv->null && inv->w > 0 && inv->h > 0) {
            UINT32 x0 = (UINT32)(inv->x < 0 ? 0 : inv->x);
            UINT32 y0 = (UINT32)(inv->y < 0 ? 0 : inv->y);
            UINT32 x1 = x0 + (UINT32)inv->w;
            UINT32 y1 = y0 + (UINT32)inv->h;
            if (!h->dirtyAny) {
                h->dirtyX0 = x0; h->dirtyY0 = y0;
                h->dirtyX1 = x1; h->dirtyY1 = y1;
                h->dirtyAny = TRUE;
            } else {
                if (x0 < h->dirtyX0) h->dirtyX0 = x0;
                if (y0 < h->dirtyY0) h->dirtyY0 = y0;
                if (x1 > h->dirtyX1) h->dirtyX1 = x1;
                if (y1 > h->dirtyY1) h->dirtyY1 = y1;
            }
        } else {
            h->dirtyAll = TRUE;      /* unknown region -- copy everything */
        }
    }

    /* THROTTLE. EndPaint fires once per DAMAGED REGION, not once per frame --
     * measured at ~280/second on an active desktop. Publishing a full 1920x1080
     * copy each time is 8 MB per publish, so about 2 GB/s of memcpy: enough to
     * saturate memory bandwidth on this machine and starve everything else. The
     * symptom was a cursor that jumped and a keyboard that stopped responding,
     * which looks like an input bug and is not.
     *
     * The panel cannot show more than the display refresh anyway, so cap at
     * ~60/s. Intermediate regions are not lost -- they have already been drawn
     * into the GDI buffer, so the next publish carries them. */
    DWORD nowTick = GetTickCount();
    BOOL  due = (nowTick - h->lastPublish) >= 16;

    if (due && hydra_open_pixels(h)) {
        h->lastPublish = nowTick;
        const UINT32 w = (UINT32)gdi->width, ht = (UINT32)gdi->height;
        if (w <= HYDRA_PIX_MAX_W && ht <= HYDRA_PIX_MAX_H &&
            FreeRDPGetBytesPerPixel(gdi->dstFormat) == 4)
        {
            h->pixHdr->seq = h->pixHdr->seq + 1;      /* odd: write in flight */
            MemoryBarrier();
            h->pixHdr->width  = w;
            h->pixHdr->height = ht;
            h->pixHdr->pitch  = w * 4;

            /* FULL-FRAME COPY.
             *
             * A damaged-rectangle copy was tried and removed. Two optimisations
             * that were each defensible alone were wrong together: the ring is
             * persistent, so pixels the composited cursor painted were never
             * restored unless the old cursor rectangle was copied too, and every
             * refinement to keep them in step made the code harder to reason
             * about while the picture stayed visibly wrong.
             *
             * The 60fps throttle already removed two thirds of the bandwidth,
             * which was the actual problem. Copying 8 MB sixty times a second is
             * affordable; copying the wrong 8 MB is not.
             *
             * Both sides are BGRA32, so this is a straight copy with no
             * conversion. Row by row because the GDI stride need not equal
             * width*4. */
            const BYTE* src = gdi->primary_buffer;
            BYTE*       dst = h->pixData;
            for (UINT32 y = 0; y < ht; ++y) {
                memcpy(dst + (size_t)y * w * 4,
                       src + (size_t)y * gdi->stride,
                       (size_t)w * 4);
            }
            UINT32 rh = ht;
            h->copiedRows += rh;

            /* Cursor last, inside the seqlock, over the whole frame rather than
             * the damaged rect -- it moves independently of what was repainted,
             * so restricting it to the damage region would leave it behind. */
            hydra_composite_pointer(h, w, ht);
            MemoryBarrier();
            h->pixHdr->seq = h->pixHdr->seq + 1;      /* even: snapshot complete */
            h->published++;
        }
        else if (!h->pixWarned) {
            h->pixWarned = TRUE;
            L("frame is %ux%u fmt 0x%08X -- outside what the ring carries "
              "(max %ux%u, 4 bytes/px); not publishing",
              w, ht, gdi->dstFormat, HYDRA_PIX_MAX_W, HYDRA_PIX_MAX_H);
        }
    }

    else if (!due) {
        h->skipped++;
    }

    if (nowTick - h->lastLog >= 5000) {
        h->lastLog = nowTick;
        if (h->curImg && !h->curHavePos)
            L("cursor: image but no position yet -- waiting for agent:%s to publish "
              "one (is the Hydra service running with the agent up?)", h->seat);
        L("seat %s: %llu paints, %llu published, %llu coalesced, %llu rows copied%s",
          h->seat,
          (unsigned long long)h->paints, (unsigned long long)h->published,
          (unsigned long long)h->skipped, (unsigned long long)h->copiedRows,
          h->pixHdr ? "" : "  (no pixel ring -- is the Hydra service running?)");
    }
    return TRUE;
}

static BOOL hydra_begin_paint(rdpContext* context)
{
    if (g_gdiBeginPaint && !g_gdiBeginPaint(context)) return FALSE;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * POINTER
 *
 * Registering our own rdpPointer callbacks replaces FreeRDP's default software
 * pointer entirely: it stops drawing into the framebuffer, and we get the cursor
 * images and positions as data instead.
 * ------------------------------------------------------------------------- */
typedef struct {
    rdpPointer pointer;        /* MUST be first */
    BYTE*      img;            /* BGRA */
    UINT32     w, h, hotX, hotY;
} HydraPointer;

static BOOL hydra_pointer_new(rdpContext* context, rdpPointer* p)
{
    HydraPointer* hp = (HydraPointer*)p;
    if (!p->width || !p->height) return TRUE;

    size_t bytes = (size_t)p->width * p->height * 4;
    hp->img = (BYTE*)calloc(1, bytes);
    if (!hp->img) return FALSE;

    /* Converts all three pointer encodings -- colour, masked-colour and
     * monochrome -- into straight BGRA, so the compositing below does not have
     * to care which arrived. session_capture had to handle those three formats
     * by hand; here the library does it. */
    if (!freerdp_image_copy_from_pointer_data(
            hp->img, PIXEL_FORMAT_BGRA32, 0, 0, 0,
            p->width, p->height,
            p->xorMaskData, p->lengthXorMask,
            p->andMaskData, p->lengthAndMask,
            p->xorBpp, &context->gdi->palette))
    {
        free(hp->img); hp->img = NULL;
        return FALSE;
    }
    hp->w = p->width; hp->h = p->height;
    hp->hotX = p->xPos; hp->hotY = p->yPos;
    return TRUE;
}

static void hydra_pointer_free(rdpContext* context, rdpPointer* p)
{
    (void)context;
    HydraPointer* hp = (HydraPointer*)p;
    free(hp->img);
    hp->img = NULL;
}

static BOOL hydra_pointer_set(rdpContext* context, rdpPointer* p)
{
    HydraContext* h  = (HydraContext*)context;
    HydraPointer* hp = (HydraPointer*)p;
    EnterCriticalSection(&h->curLock);
    free(h->curImg);
    h->curImg = NULL;
    if (hp->img && hp->w && hp->h) {
        size_t bytes = (size_t)hp->w * hp->h * 4;
        h->curImg = (BYTE*)malloc(bytes);
        if (h->curImg) memcpy(h->curImg, hp->img, bytes);
        h->curW = hp->w; h->curH = hp->h;
        h->curHotX = hp->hotX; h->curHotY = hp->hotY;
        h->curVisible = TRUE;
        {
            static BOOL saidSet = FALSE;
            if (!saidSet) { saidSet = TRUE;
                L("pointer IMAGE received (%ux%u, hotspot %u,%u)",
                  hp->w, hp->h, hp->hotX, hp->hotY); }
        }
    }
    LeaveCriticalSection(&h->curLock);
    return TRUE;
}

static BOOL hydra_pointer_set_null(rdpContext* context)
{
    HydraContext* h = (HydraContext*)context;
    EnterCriticalSection(&h->curLock);
    h->curVisible = FALSE;     /* app hid the cursor -- honour that */
    LeaveCriticalSection(&h->curLock);
    return TRUE;
}

static BOOL hydra_pointer_set_default(rdpContext* context)
{
    (void)context;
    return TRUE;               /* keep whatever we last had */
}

static BOOL hydra_pointer_set_position(rdpContext* context, UINT32 x, UINT32 y)
{
    HydraContext* h = (HydraContext*)context;
    EnterCriticalSection(&h->curLock);
    h->curX = (INT32)x; h->curY = (INT32)y;
    /* Without this the cursor is never drawn at all -- hydra_composite_pointer
     * refuses to paint until a real position has arrived, precisely so it does
     * not appear at (0,0) before the first update. */
    h->curHavePos = TRUE;
    LeaveCriticalSection(&h->curLock);
    {
        static BOOL saidPos = FALSE;
        if (!saidPos) { saidPos = TRUE; L("pointer POSITION updates arriving (%u,%u)", x, y); }
    }
    return TRUE;
}

static void hydra_register_pointer(rdpContext* context)
{
    rdpPointer p;
    memset(&p, 0, sizeof(p));
    p.size        = sizeof(HydraPointer);
    p.New         = hydra_pointer_new;
    p.Free        = hydra_pointer_free;
    p.Set         = hydra_pointer_set;
    p.SetNull     = hydra_pointer_set_null;
    p.SetDefault  = hydra_pointer_set_default;
    p.SetPosition = hydra_pointer_set_position;
    graphics_register_pointer(context->graphics, &p);
}

/* Alpha-blend the current pointer into the ring, clipped to the frame.
 * Called immediately before the seqlock closes, so a reader never sees a frame
 * with the cursor half-drawn. */
static void hydra_composite_pointer(HydraContext* h, UINT32 w, UINT32 ht)
{
    /* Take the position from the shared header, published by agent:<seat> from
     * inside the session. RDP will not tell us -- see hydra_ipc.h. */
    if (h->pixHdr && h->pixHdr->curSeq) {
        EnterCriticalSection(&h->curLock);
        h->curX = h->pixHdr->curX;
        h->curY = h->pixHdr->curY;
        h->curHavePos = TRUE;
        LeaveCriticalSection(&h->curLock);
    }

    EnterCriticalSection(&h->curLock);
    if (!h->curVisible || !h->curHavePos || !h->curImg || !h->curW || !h->curH) {
        LeaveCriticalSection(&h->curLock);
        return;
    }

    INT32 ox = h->curX - (INT32)h->curHotX;
    INT32 oy = h->curY - (INT32)h->curHotY;

    for (UINT32 cy = 0; cy < h->curH; ++cy) {
        INT32 dy = oy + (INT32)cy;
        if (dy < 0 || dy >= (INT32)ht) continue;
        const BYTE* srow = h->curImg + (size_t)cy * h->curW * 4;
        BYTE*       drow = h->pixData + (size_t)dy * w * 4;
        for (UINT32 cx = 0; cx < h->curW; ++cx) {
            INT32 dx = ox + (INT32)cx;
            if (dx < 0 || dx >= (INT32)w) continue;
            const BYTE* sp = srow + (size_t)cx * 4;
            BYTE*       dp = drow + (size_t)dx * 4;
            BYTE a = sp[3];
            if (!a) continue;                       /* fully transparent */
            if (a == 255) { memcpy(dp, sp, 4); continue; }
            dp[0] = (BYTE)((sp[0] * a + dp[0] * (255 - a)) / 255);
            dp[1] = (BYTE)((sp[1] * a + dp[1] * (255 - a)) / 255);
            dp[2] = (BYTE)((sp[2] * a + dp[2] * (255 - a)) / 255);
        }
    }
    h->prevCurX = ox; h->prevCurY = oy;
    h->prevCurW = h->curW; h->prevCurH = h->curH;
    h->prevCurDrawn = TRUE;

    LeaveCriticalSection(&h->curLock);
}

/* Wire the RDP8 graphics pipeline into GDI when its channel comes up.
 *
 * Without this the session falls back to plain bitmap updates -- no RemoteFX, no
 * H.264 -- and video looks blocky and smeared, because every changed region is
 * sent as raw or RLE-compressed bitmap rather than as a video codec. The flag
 * alone is not enough: the gfx channel has to be handed to gdi_graphics_pipeline
 * _init or the decoded output never reaches the framebuffer we publish from.
 *
 * Every stock client does this in its channel-connected handler; it is not
 * optional plumbing. */
/* ---------------------------------------------------------------------------
 * GFX WINDOW-MAPPING STUBS -- the reason gfx crashed.
 *
 * gdi_graphics_pipeline_init() is a one-line wrapper:
 *
 *     return gdi_graphics_pipeline_init_ex(gdi, gfx, nullptr, nullptr, nullptr);
 *
 * It passes NULL for MapWindowForSurface, UnmapWindowForSurface and
 * UpdateSurfaceArea. Init itself is defensive and succeeds, so the log shows the
 * pipeline attaching happily -- and then the first surface operation calls one
 * of those null pointers. That is exactly the crash we kept getting:
 *
 *     FATAL: exception 0xC0000005 at 0000000000000000
 *
 * A call to address zero. It was never the codec: RemoteFX failed identically to
 * AVC, which is what finally ruled the H.264 theory out.
 *
 * The stock clients call init_ex with real callbacks because they have windows
 * to map surfaces onto. We have none -- that is the whole point of a headless
 * client -- so the callbacks need only EXIST and succeed. Doing nothing is the
 * correct behaviour here, not a shortcut.
 * ------------------------------------------------------------------------- */
static UINT hydra_gfx_map_window(RdpgfxClientContext* context, UINT16 surfaceID,
                                 UINT64 windowID)
{
    (void)context; (void)surfaceID; (void)windowID;
    return CHANNEL_RC_OK;      /* headless: no window to map a surface onto */
}

static UINT hydra_gfx_unmap_window(RdpgfxClientContext* context, UINT64 windowID)
{
    (void)context; (void)windowID;
    return CHANNEL_RC_OK;
}

static UINT hydra_gfx_update_surface_area(RdpgfxClientContext* context, UINT16 surfaceId,
                                          UINT32 nrRects, const RECTANGLE_16* rects)
{
    /* The decoded pixels are already in gdi->primary_buffer by the time this is
     * called; our EndPaint publishes the whole frame from there. Nothing to do
     * per-rectangle. */
    (void)context; (void)surfaceId; (void)nrRects; (void)rects;
    return CHANNEL_RC_OK;
}

static void hydra_on_channel_connected(void* context, const ChannelConnectedEventArgs* e)
{
    rdpContext* ctx = (rdpContext*)context;
    if (0) {   /* gfx must go to the COMMON handler -- the SDL client never intercepts it */
        if (!gdi_graphics_pipeline_init_ex(ctx->gdi, (RdpgfxClientContext*)e->pInterface,
                                           hydra_gfx_map_window,
                                           hydra_gfx_unmap_window,
                                           hydra_gfx_update_surface_area))
        {
            L("gdi_graphics_pipeline_init_ex failed");
            return;
        }
        L("graphics pipeline attached -- video should decode properly now");
    }
    else
        freerdp_client_OnChannelConnectedEventHandler(context, e);   /* required: stock clients do exactly this */
}

static void hydra_on_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e)
{
    rdpContext* ctx = (rdpContext*)context;
    if (0)
        gdi_graphics_pipeline_uninit(ctx->gdi, (RdpgfxClientContext*)e->pInterface);
    else
        freerdp_client_OnChannelDisconnectedEventHandler(context, e);
}

/* Called BEFORE the connection is negotiated.
 *
 * freerdp_client_load_addins registers the channel plugins the client will
 * advertise. Skipping it lets authentication succeed and then hangs the
 * capability exchange -- the server waits for channel declarations that never
 * arrive, and the connect fails with ERRCONNECT_ACTIVATION_TIMEOUT nine seconds
 * later. Nothing in that error hints at channels. */
static BOOL hydra_pre_connect(freerdp* instance)
{
    rdpContext* c = instance->context;

    PubSub_SubscribeChannelConnected(c->pubSub, hydra_on_channel_connected);
    PubSub_SubscribeChannelDisconnected(c->pubSub, hydra_on_channel_disconnected);

    if (!freerdp_client_load_addins(c->channels, c->settings)) {
        L("load_addins failed");
        return FALSE;
    }
    return TRUE;
}

/* Called once the connection is up and settings are final. GDI must be
 * initialised here or there is nowhere for frames to be decoded to. */
/* Open the shared pixel ring. hydrad creates it -- a Global\ section needs a
 * privilege the seat user does not have, the same constraint session_capture
 * works under. Failure is not fatal: the client still holds the session open,
 * which is useful on its own. */
static BOOL hydra_open_pixels(HydraContext* h)
{
    if (h->pixHdr) return TRUE;
    wchar_t name[128];
    hydra_pixels_name(name, 128, h->seat);
    h->pixMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h->pixMap) return FALSE;
    void* v = MapViewOfFile(h->pixMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!v) { CloseHandle(h->pixMap); h->pixMap = NULL; return FALSE; }
    h->pixHdr  = (HydraSeatPixels*)v;
    h->pixData = (BYTE*)v + sizeof(HydraSeatPixels);
    L("pixel transport opened -- mirror will display these frames");
    return TRUE;
}

static BOOL hydra_post_connect(freerdp* instance)
{
    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
        L("gdi_init failed");
        return FALSE;
    }
    g_gdiBeginPaint = instance->context->update->BeginPaint;
    g_gdiEndPaint   = instance->context->update->EndPaint;
    instance->context->update->BeginPaint = hydra_begin_paint;
    instance->context->update->EndPaint   = hydra_end_paint;
    /* BISECT: skip our pointer registration when gfx is on -- gdi_graphics_pipeline_init
     * installs its own graphics module and may not tolerate a replaced pointer. */
    { char gv[8] = {0}; DWORD n = GetEnvironmentVariableA("HYDRA_GFX", gv, sizeof(gv));
      if (!(n > 0 && gv[0] != 0)) hydra_register_pointer(instance->context);   /* skip for ANY gfx value, not just '1' */
      else L("pointer registration SKIPPED (gfx bisect)"); }
    L("connected; GDI ready (pointer handled by us, not drawn into the buffer)");
    /* ASK FOR THE WHOLE DESKTOP. After connecting the server sends only CHANGES; on an idle desktop that is nothing, so our framebuffer stays black. Connecting a second client made the picture appear because its arrival forced the full refresh we never asked for. */
    { RECTANGLE_16 r; r.left = 0; r.top = 0; r.right = (UINT16)freerdp_settings_get_uint32(instance->context->settings, FreeRDP_DesktopWidth); r.bottom = (UINT16)freerdp_settings_get_uint32(instance->context->settings, FreeRDP_DesktopHeight); if (instance->context->update->RefreshRect) { instance->context->update->RefreshRect(instance->context, 1, &r); L("requested a full refresh (%ux%u)", r.right, r.bottom); } }
    return TRUE;
}

static void hydra_post_disconnect(freerdp* instance)
{
    gdi_free(instance);
    L("disconnected");
}

/* The wrapped listener presents a self-signed certificate; accept it rather than
 * prompting, since there is no user at this end. */
static DWORD hydra_verify_cert(freerdp* instance, const char* host, UINT16 port,
                               const char* common_name, const char* subject,
                               const char* issuer, const char* fingerprint, DWORD flags)
{
    (void)instance; (void)host; (void)port; (void)common_name;
    (void)subject; (void)issuer; (void)fingerprint; (void)flags;
    return 2;   /* accept permanently */
}

/* ---------------------------------------------------------------------------
 * Client context plumbing.
 *
 * FreeRDP 3.x expects a client to be created through freerdp_client_context_new
 * with RDP_CLIENT_ENTRY_POINTS, not via bare freerdp_new + freerdp_context_new.
 * The entry-points path initialises client-common state that the raw path
 * leaves empty -- and the failure mode is thoroughly misleading: connecting to
 * a literal IP reports ERRCONNECT_DNS_NAME_NOT_FOUND, as though the address
 * were a hostname that could not be resolved, when in fact the settings the
 * resolver reads were never fully set up. sdl-freerdp works against the same
 * address precisely because it takes this path.
 * ------------------------------------------------------------------------- */
static BOOL hydra_client_new(freerdp* instance, rdpContext* context)
{
    (void)context;
    instance->PreConnect          = hydra_pre_connect;
    instance->PostConnect         = hydra_post_connect;
    instance->PostDisconnect      = hydra_post_disconnect;
    instance->VerifyCertificateEx = hydra_verify_cert;
    return TRUE;
}

static void hydra_client_free(freerdp* instance, rdpContext* context)
{
    (void)instance; (void)context;
}

static int hydra_client_start(rdpContext* context) { (void)context; return 0; }
static int hydra_client_stop(rdpContext* context)  { (void)context; return 0; }

/* WINSOCK.
 *
 * Without WSAStartup, getaddrinfo fails for EVERY address -- including literal
 * IPs -- and FreeRDP surfaces that as ERRCONNECT_DNS_NAME_NOT_FOUND. Which reads
 * like a name-resolution problem and sent me looking at hostnames, settings and
 * context construction in turn, none of which were wrong. The tell was that
 * 127.0.0.1 failed exactly like 127.0.0.2: nothing to do with the address.
 *
 * Stock clients do this in their GlobalInit; I had left that callback NULL. */
static BOOL hydra_global_init(void)
{
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) { L("WSAStartup failed: %d", rc); return FALSE; }
    return TRUE;
}

static void hydra_global_uninit(void)
{
    WSACleanup();
}

static int hydra_entry(RDP_CLIENT_ENTRY_POINTS* p)
{
    p->Version         = RDP_CLIENT_INTERFACE_VERSION;
    p->Size            = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
    p->ContextSize     = sizeof(HydraContext);
    p->ClientNew       = hydra_client_new;
    p->ClientFree      = hydra_client_free;
    p->ClientStart     = hydra_client_start;
    p->ClientStop      = hydra_client_stop;
    p->GlobalInit      = hydra_global_init;
    p->GlobalUninit    = hydra_global_uninit;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: hydrardp <seat> <user> [password] [host]\n"
            "  e.g. hydrardp B teacher '' 127.0.0.2\n"
            "MILESTONE 1: connects and counts frames. Publishes nothing yet.\n");
        return 2;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    /* The one that matters: a crash which skips the disconnect leaves the RDP
     * wrapper holding a session it cannot clean up, and every later connection
     * fails until the machine is REBOOTED. */
    SetUnhandledExceptionFilter(hydra_crash_filter);

    const char* seat = argv[1];
    const char* user = argv[2];
    /* Password: prompt unless one was passed. A password in argv is visible to
     * every process on the machine and lands in shell history, which is a poor
     * trade for saving one prompt. */
    char passbuf[256] = {0};
    const char* pass = (argc >= 4) ? argv[3] : NULL;
    if (!pass || !*pass) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hIn, &mode);
        SetConsoleMode(hIn, mode & ~(DWORD)ENABLE_ECHO_INPUT);   /* no echo */
        fprintf(stderr, "password for %s: ", argv[2]);
        fflush(stderr);
        if (fgets(passbuf, sizeof(passbuf), stdin)) {
            size_t n = strlen(passbuf);
            while (n && (passbuf[n-1] == '\n' || passbuf[n-1] == '\r')) passbuf[--n] = 0;
        }
        SetConsoleMode(hIn, mode);
        fprintf(stderr, "\n");
        pass = passbuf;
    }
    const char* host = (argc >= 5) ? argv[4] : "127.0.0.2";

    RDP_CLIENT_ENTRY_POINTS ep;
    memset(&ep, 0, sizeof(ep));
    hydra_entry(&ep);

    rdpContext* ctx = freerdp_client_context_new(&ep);
    if (!ctx) { L("freerdp_client_context_new failed"); return 1; }
    freerdp* inst = ctx->instance;
    g_inst = inst;                     /* teardown paths need it from here on */

    HydraContext* h = (HydraContext*)ctx;
    strncpy(h->seat, seat, sizeof(h->seat) - 1);
    InitializeCriticalSection(&h->curLock);

    /* LET FREERDP PARSE ITS OWN COMMAND LINE.
     *
     * Setting fields by hand got as far as authentication and then hung the
     * capability exchange -- ERRCONNECT_ACTIVATION_TIMEOUT, nine seconds of
     * nothing. The cause is not one missing field: parse_command_line sets
     * dozens of defaults (RDP version, performance flags, codec and channel
     * negotiation, colour handling) that a client is expected to have, and
     * sdl-freerdp works against this very server precisely because it goes
     * through here.
     *
     * Guessing which of those defaults matters is a poor use of time when we can
     * simply take the same path. Build an argv and hand it over. */
    int prc = 0;
    char vArg[64], uArg[128], pArg[280], wArg[32], hArg[32];
    snprintf(vArg, sizeof(vArg), "/v:%s", host);
    snprintf(uArg, sizeof(uArg), "/u:%s", user);
    snprintf(pArg, sizeof(pArg), "/p:%s", pass ? pass : "");
    snprintf(wArg, sizeof(wArg), "/w:%d", 1920);
    snprintf(hArg, sizeof(hArg), "/h:%d", 1080);

    char* fargv[] = {
        (char*)"hydrardp",
        vArg, uArg, pArg, wArg, hArg,
        (char*)"/d:",              /* empty domain */
        (char*)"/cert:ignore",
        (char*)"/bpp:32",
        (char*)"+auto-reconnect",
        (char*)"/gdi:sw",          /* software GDI: pixels in a buffer we can read */
        /* RDP8 graphics pipeline. Without it the server sends plain bitmap
         * updates and video is visibly blocky -- viewers described it as
         * "glitchy", which is exactly what uncompressed region updates of a
         * moving image look like. */
        /* Decline multitransport (the side UDP channel).
         *
         * The trace showed the connection reaching
         * CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_REQUEST, answering
         * SEC_TRANSPORT_REQ with SEC_TRANSPORT_RSP, and then stalling for eight
         * seconds until ACTIVATION_TIMEOUT. The server was waiting on a transport we
         * never bring up. Nothing to do with gfx -- which is why four hypotheses
         * about the graphics pipeline all missed. */
        (char*)"-multitransport",
        (char*)"/network:lan",     /* don't let autodetect throttle quality */
    };
    int fargc = (int)(sizeof(fargv) / sizeof(fargv[0]));

    /* RDP8 graphics pipeline: opt-in via HYDRA_GFX=1.
     *
     * It is the difference between video arriving as a codec stream and arriving
     * as raw region updates -- the latter is what onlookers described as
     * "glitchy". But it also changes how the framebuffer is managed, so keep it
     * switchable rather than making a quality improvement able to take the whole
     * client down. */
    char* fargvGfx[32];
    char gfxEnv[24] = {0};
    DWORD gl = GetEnvironmentVariableA("HYDRA_GFX", gfxEnv, sizeof(gfxEnv));
    if (gl > 0 && gfxEnv[0] != 0) {
        int n = 0;
        for (; n < fargc; ++n) fargvGfx[n] = fargv[n];
        /* HYDRA_GFX selects the CODEC, not just on/off:
         *   1            -> /gfx            (server picks; usually AVC/H.264)
         *   RFX          -> /gfx:RFX        (RemoteFX -- no H.264)
         *   progressive  -> /gfx:progressive
         *   AVC420 etc.  -> passed through
         *
         * This matters because libfreerdp here is built with
         * WITH_VAAPI_H264_ENCODING=ON, which it warns is EXPERIMENTAL and "might
         * crash the application" -- and the crash we get is a call through a null
         * pointer immediately after the pipeline attaches, which is what a
         * half-initialised codec looks like. FreeRDP issue 12221 is the same
         * shape: gfx plus an experimental VAAPI build, crashing shortly after
         * connect. Avoiding H.264 avoids that path entirely. */
        static char gfxArg[32];
        if (gfxEnv[0] == '1' && gfxEnv[1] == 0)
            strcpy(gfxArg, "/gfx");
        else
            snprintf(gfxArg, sizeof(gfxArg), "/gfx:%s", gfxEnv);
        fargvGfx[n++] = gfxArg;
        L("graphics pipeline requested: %s", gfxArg);
        prc = freerdp_client_settings_parse_command_line(ctx->settings, n, fargvGfx, FALSE);
    } else {
        prc = freerdp_client_settings_parse_command_line(ctx->settings, fargc, fargv, FALSE);
    }

    if (prc != 0) {
        L("parse_command_line failed: %d", prc);
        return 1;
    }

    /* Wipe the password out of our argv copy now it has been consumed. */
    memset(pArg, 0, sizeof(pArg));

    L("target: %s:%u as '%s'",
      freerdp_settings_get_string(ctx->settings, FreeRDP_ServerHostname),
      freerdp_settings_get_uint32(ctx->settings, FreeRDP_ServerPort),
      freerdp_settings_get_string(ctx->settings, FreeRDP_Username));

    if (!freerdp_connect(inst)) {
        L("connect failed: 0x%08X", freerdp_get_last_error(inst->context));
        return 1;
    }

    L("seat %s: connected to %s as %s -- counting frames, Ctrl+C to stop",
      seat, host, user);

    /* Plain blocking loop: wait on FreeRDP's handles, drain events. No window,
     * no message pump, nothing to be occluded. */
    while (g_run && !freerdp_shall_disconnect_context(inst->context)) {
        HANDLE handles[64];
        DWORD n = freerdp_get_event_handles(inst->context, handles,
                                            sizeof(handles) / sizeof(handles[0]));
        if (n == 0) { L("get_event_handles failed"); break; }

        if (WaitForMultipleObjects(n, handles, FALSE, 16) == WAIT_FAILED) {
            L("wait failed"); break;
        }
        /* Cursor moved but nothing repainted: republish so the composited
         * pointer actually moves. The 16ms throttle inside still applies. */
        if (g_curMoved) { g_curMoved = FALSE; hydra_end_paint(inst->context); }

        /* Publish on a TIMER, not on paint callbacks. With the graphics pipeline,

         * content arrives as SURFACES that gfx blits to the output on its own

         * schedule -- EndPaint is not the signal. Sampling only on EndPaint gave a

         * mostly-empty frame with one white box where a surface happened to land.

         * The 60fps throttle inside makes this cheap regardless of how often we ask. */

        hydra_end_paint(inst->context);


        if (!freerdp_check_event_handles(inst->context)) {
            if (freerdp_get_last_error(inst->context) == FREERDP_ERROR_SUCCESS)
                L("check_event_handles failed");
            break;
        }
    }

    L("seat %s: %llu paints, %llu published",
      seat, (unsigned long long)h->paints, (unsigned long long)h->published);
    if (h->pixHdr) { UnmapViewOfFile(h->pixHdr); h->pixHdr = NULL; }
    if (h->pixMap) { CloseHandle(h->pixMap);     h->pixMap = NULL; }
    hydra_teardown("normal exit");
    freerdp_client_context_free(ctx);
    return 0;
}
























