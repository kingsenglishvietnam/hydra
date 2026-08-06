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

/* Forward declaration: the paint callback publishes frames, and it sits above
 * the function that opens the ring. */
static BOOL hydra_open_pixels(HydraContext* h);

static volatile BOOL g_run = TRUE;
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; g_run = FALSE; return TRUE; }

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

    /* PUBLISH. Under a seqlock: odd while writing, even when the snapshot is
     * complete, so a reader either gets a whole frame or notices and retries.
     * Identical protocol to session_capture, because mirror is unchanged. */
    if (hydra_open_pixels(h)) {
        const UINT32 w = (UINT32)gdi->width, ht = (UINT32)gdi->height;
        if (w <= HYDRA_PIX_MAX_W && ht <= HYDRA_PIX_MAX_H &&
            FreeRDPGetBytesPerPixel(gdi->dstFormat) == 4)
        {
            h->pixHdr->seq = h->pixHdr->seq + 1;      /* odd: write in flight */
            MemoryBarrier();
            h->pixHdr->width  = w;
            h->pixHdr->height = ht;
            h->pixHdr->pitch  = w * 4;

            /* Both sides are BGRA32 -- the client negotiated PIXEL_FORMAT_BGRA32
             * and the ring is BGRA32 -- so this is a straight copy with no
             * conversion. Row by row because the GDI buffer's stride need not
             * equal width*4. */
            const BYTE* src = gdi->primary_buffer;
            BYTE*       dst = h->pixData;
            for (UINT32 y = 0; y < ht; ++y) {
                memcpy(dst, src, (size_t)w * 4);
                src += gdi->stride;
                dst += (size_t)w * 4;
            }
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

    DWORD now = GetTickCount();
    if (now - h->lastLog >= 5000) {
        h->lastLog = now;
        L("seat %s: %llu paints, %llu published%s", h->seat,
          (unsigned long long)h->paints, (unsigned long long)h->published,
          h->pixHdr ? "" : "  (no pixel ring -- is the Hydra service running?)");
    }
    return TRUE;
}

static BOOL hydra_begin_paint(rdpContext* context) { (void)context; return TRUE; }

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
    instance->context->update->BeginPaint = hydra_begin_paint;
    instance->context->update->EndPaint   = hydra_end_paint;
    L("connected; GDI ready");
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

    HydraContext* h = (HydraContext*)ctx;
    strncpy(h->seat, seat, sizeof(h->seat) - 1);

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
    };
    int fargc = (int)(sizeof(fargv) / sizeof(fargv[0]));

    int prc = freerdp_client_settings_parse_command_line(ctx->settings, fargc, fargv, FALSE);
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

        if (WaitForMultipleObjects(n, handles, FALSE, 250) == WAIT_FAILED) {
            L("wait failed"); break;
        }
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
    freerdp_disconnect(inst);
    freerdp_client_context_free(ctx);
    return 0;
}
