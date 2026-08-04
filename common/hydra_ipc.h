/* hydra_ipc.h  --  the contract that connects iddseat (producer) to mirror
 *                  (consumer) across the session boundary.
 *
 * THE PROBLEM THIS SOLVES (was ARCHITECTURE.md risk #2)
 *   iddseat runs inside Wudfhost.exe (UMDF host, session 0). mirror runs in the
 *   console session (seat A). They must share a D3D11 texture *across sessions*.
 *   The samples show CreateSharedHandle -> DuplicateHandle into the peer, which
 *   is painful across the session boundary and needs the peer's PID + security.
 *
 * THE FIX
 *   Don't duplicate a handle at all. Create the shared resource as a *named* NT
 *   handle in the Global\ namespace and open it by name on the other side:
 *     producer: IDXGIResource1::CreateSharedHandle(..., name)   name = kSurfaceName
 *     consumer: ID3D11Device1::OpenSharedResourceByName(name)
 *   The Global\ prefix makes the name reachable from any session (both processes
 *   have SeCreateGlobalPrivilege: iddseat is SYSTEM, mirror is launched by the
 *   SYSTEM service into the console session). No PID, no DuplicateHandle, no
 *   per-frame re-share.
 *
 *   A second tiny named object carries the metadata the consumer needs *before*
 *   it can open the surface: the LUID to match (so it puts its device on the same
 *   GPU -> zero-copy), the dimensions/format, a ready flag, and a monotonically
 *   increasing frame counter. That is HydraSeatMeta below, a shared file mapping.
 *
 * SYNCHRONISATION
 *   The surface is created with SHARED_KEYEDMUTEX | SHARED_NTHANDLE. Producer and
 *   consumer both acquire/release key 0 (mutual exclusion, "latest frame wins").
 *   We deliberately do NOT ping-pong two keys: a strict two-key handshake would
 *   stall the IddCx swapchain thread whenever the mirror is a frame behind. The
 *   mirror paces on its own vsync, grabs whatever complete frame is currently in
 *   the texture, and uses `frame` in the metadata to skip re-presenting a
 *   duplicate. Tear-free, and the producer never blocks on a slow consumer.
 *
 * This header is C-friendly (plain struct + name builders); both the C++ driver
 * and C++ mirror include it, and hydrad uses the name builders when it launches
 * the mirror so everyone agrees on the strings.
 */
#ifndef HYDRA_IPC_H
#define HYDRA_IPC_H

#include <stdint.h>
#include <wchar.h>

/* Bump if the shared-memory layout ever changes; both sides check it. */
#define HYDRA_IPC_VERSION 1

/* Metadata published by iddseat, read by mirror. Lives in a shared file mapping
 * named by hydra_meta_name(). Written by the producer whenever a swapchain is
 * (re)assigned; polled by the consumer. All fields little-endian (same machine). */
typedef struct HydraSeatMeta {
    uint32_t version;      /* == HYDRA_IPC_VERSION */
    volatile uint32_t ready; /* 0 = surface not present; 1 = surface open-able   */
    uint32_t width;        /* surface dimensions in pixels                        */
    uint32_t height;
    uint32_t dxgiFormat;   /* DXGI_FORMAT of the shared texture (BGRA8 typ.)     */
    uint32_t luidLow;      /* adapter LUID the producer used (match for 0-copy)  */
    int32_t  luidHigh;
    volatile uint64_t frame; /* increments each published frame; consumer dedups  */
    uint32_t generation;   /* increments each swapchain (re)assign; forces reopen */
    uint32_t _pad;
} HydraSeatMeta;

/* ---------------------------------------------------------------------------
 * PIXEL TRANSPORT (added after the cross-session wall was hit)
 *
 * The design above shares a D3D11 texture by name. That works when producer and
 * consumer are in the SAME session -- the original topology, with iddseat in
 * session 0 and mirror in the console session. It does NOT work now that
 * session_capture runs inside the seat's own RDP session: D3D11 shared textures
 * do not cross a Terminal Services session boundary, because each session has
 * its own GPU context. OpenSharedResourceByName resolves the name and then fails
 * the handover with E_INVALIDARG (0x80070057), no matter what access flags are
 * requested. Hours were spent on the flags before the session split was seen.
 *
 * Plain shared MEMORY does cross sessions -- the metadata section above already
 * proves it. So frames now travel as pixels: the producer reads its composited
 * texture back to the CPU and writes it here; the consumer uploads it to its own
 * texture. Costs one readback and one upload of a frame per present (~8 MB at
 * 1080p), which is well within budget and buys a transport that actually works.
 *
 * TEARING is handled with a seqlock rather than a mutex: the producer bumps `seq`
 * to an ODD value before writing and to the next EVEN value after. The consumer
 * reads seq, copies, reads seq again, and retries if it changed or was odd. No
 * kernel object, and a slow consumer never blocks the producer.
 * ------------------------------------------------------------------------- */
#define HYDRA_PIX_MAX_W  3840u
#define HYDRA_PIX_MAX_H  2160u
#define HYDRA_PIX_BYTES  ((size_t)HYDRA_PIX_MAX_W * (size_t)HYDRA_PIX_MAX_H * 4u)

typedef struct HydraSeatPixels {
    volatile uint64_t seq;     /* odd = write in flight, even = stable snapshot */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;            /* bytes per row as written here (== width*4)    */
    uint32_t _pad;
    /* width*height*4 bytes of BGRA follow immediately */
} HydraSeatPixels;

#define HYDRA_PIX_TOTAL (sizeof(HydraSeatPixels) + HYDRA_PIX_BYTES)

static inline void hydra_pixels_name(wchar_t* out, size_t cch, const char* seat) {
    /* e.g. Global\HydraSeat_B_pix */
    size_t i = 0;
    const wchar_t* p = L"Global\\HydraSeat_";
    while (*p && i + 1 < cch) out[i++] = *p++;
    while (seat && *seat && i + 1 < cch) out[i++] = (wchar_t)(unsigned char)*seat++;
    const wchar_t* s2 = L"_pix";
    while (*s2 && i + 1 < cch) out[i++] = *s2++;
    if (i < cch) out[i] = 0;
}


/* ---------------------------------------------------------------------------
 * AUDIO TRANSPORT -- for A/V sync.
 *
 * THE PROBLEM
 *   Video and audio take completely different routes to the seat's monitor:
 *     video: seat session -> DDA capture -> shared memory -> mirror -> panel
 *     audio: seat session -> RDP channel -> mstsc -> monitor endpoint
 *   Two independent paths with independent latency, and nothing aligning them.
 *   The RDP audio channel buffers substantially more than DDA capture does, so
 *   audio lags video. audioqualitymode only changes the CODEC inside that
 *   channel, which is why neither :0 nor :2 affected sync -- wrong layer.
 *
 * THE FIX
 *   Carry audio over the SAME shared-memory transport as the video. A capture
 *   agent in the seat's session loopback-records the session mix and writes PCM
 *   here; a render agent in the console session reads it and plays it to the
 *   monitor. Shared memory adds a few ms, against the RDP channel's tens to
 *   hundreds, so the two paths land far closer together.
 *
 *   The RDP client must then be MUTED (per-app, in the console session) or the
 *   same audio plays twice, out of phase with itself.
 *
 * RING, NOT SEQLOCK
 *   Unlike video -- where only the newest frame matters and a torn read is
 *   simply skipped -- audio must not drop samples, so this is a proper ring
 *   buffer. Single writer, single reader, so a monotonic write position and a
 *   reader-owned read position need no locking at all.
 *
 *   A reader that falls more than the ring behind has stalled; it resynchronises
 *   to the write head rather than playing stale audio, accepting one glitch
 *   instead of permanent drift.
 * ------------------------------------------------------------------------- */
#define HYDRA_AUD_RATE     48000u
#define HYDRA_AUD_CHANNELS 2u
#define HYDRA_AUD_FRAMES   48000u          /* 1 second of ring */
#define HYDRA_AUD_BYTES    ((size_t)HYDRA_AUD_FRAMES * HYDRA_AUD_CHANNELS * sizeof(float))

/* How much audio the renderer tries to keep buffered. Too small and any jitter
 * underruns; too large and we reintroduce the latency this exists to remove.
 * 40 ms is comfortably above the WASAPI engine period and still well under the
 * threshold where lip-sync becomes noticeable. */
#define HYDRA_AUD_TARGET_MS 40u

typedef struct HydraAudioRing {
    volatile uint64_t writePos;   /* total frames ever written; never wraps */
    uint32_t rate;
    uint32_t channels;
    uint32_t ringFrames;
    uint32_t running;             /* producer sets 1 while it is alive */
    /* ringFrames * channels floats follow */
} HydraAudioRing;

#define HYDRA_AUD_TOTAL (sizeof(HydraAudioRing) + HYDRA_AUD_BYTES)

static inline void hydra_audio_name(wchar_t* out, size_t cch, const char* seat) {
    size_t i = 0;
    const wchar_t* p = L"Global\\HydraSeat_";
    while (*p && i + 1 < cch) out[i++] = *p++;
    while (seat && *seat && i + 1 < cch) out[i++] = (wchar_t)(unsigned char)*seat++;
    const wchar_t* s2 = L"_aud";
    while (*s2 && i + 1 < cch) out[i++] = *s2++;
    if (i < cch) out[i] = 0;
}

/* Object names. Seat names are short ASCII (e.g. "B"); we widen inline.
 * Global\ so they cross the session boundary. Keep well under MAX_PATH. */

static inline void hydra_meta_name(wchar_t* out, size_t cch, const char* seat) {
    /* e.g. Global\HydraSeat_B_meta */
    size_t i = 0;
    const wchar_t* p = L"Global\\HydraSeat_";
    while (*p && i + 1 < cch) out[i++] = *p++;
    while (seat && *seat && i + 1 < cch) out[i++] = (wchar_t)(unsigned char)*seat++;
    const wchar_t* s = L"_meta";
    while (*s && i + 1 < cch) out[i++] = *s++;
    if (i < cch) out[i] = 0;
}

static inline void hydra_surface_name(wchar_t* out, size_t cch, const char* seat) {
    /* e.g. Global\HydraSeat_B_surf */
    size_t i = 0;
    const wchar_t* p = L"Global\\HydraSeat_";
    while (*p && i + 1 < cch) out[i++] = *p++;
    while (seat && *seat && i + 1 < cch) out[i++] = (wchar_t)(unsigned char)*seat++;
    const wchar_t* s = L"_surf";
    while (*s && i + 1 < cch) out[i++] = *s++;
    if (i < cch) out[i] = 0;
}

/* hydrad's control pipe (hydractl <-> hydrad). Single instance, message mode. */
#define HYDRA_CONTROL_PIPE L"\\\\.\\pipe\\hydra_control"

/* The single keyed-mutex key both sides use (see note above: no ping-pong). */
#define HYDRA_MUTEX_KEY 0ULL

/* How long a consumer waits on the keyed mutex before skipping a frame (ms). */
#define HYDRA_ACQUIRE_TIMEOUT_MS 8

#endif /* HYDRA_IPC_H */
