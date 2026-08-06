/* audio_bridge.c -- carry a seat's audio over shared memory instead of RDP.
 *
 * WHY
 *   Video reaches the seat's monitor by one route and audio by another:
 *       video: seat session -> DDA capture -> shared memory -> mirror -> panel
 *       audio: seat session -> RDP channel  -> mstsc        -> monitor
 *   Nothing aligns them, and the RDP audio channel buffers far more than DDA
 *   capture does, so audio lags video. Changing audioqualitymode does nothing
 *   for this -- that only selects the codec INSIDE the channel that is the
 *   problem.
 *
 *   Putting audio on the same shared-memory transport as the video removes tens
 *   to hundreds of milliseconds of RDP buffering and lands the two paths close
 *   enough together to look right.
 *
 * TWO MODES, ONE BINARY
 *   capture <seat>   -- runs IN THE SEAT'S SESSION. Loopback-records that
 *                       session's mix (its "Remote Audio" endpoint) and writes
 *                       PCM into the shared ring.
 *   render <seat> <endpoint-substr>
 *                    -- runs in the CONSOLE session. Reads the ring and plays it
 *                       to the named endpoint (the seat's monitor).
 *
 *   No feedback loop: the loopback endpoint in the seat's session is virtual --
 *   nothing is actually played there -- while the real playback happens on a
 *   different endpoint in a different session. This is the same reason
 *   endpoint-loopback failed before and works now: previously the session could
 *   reach the real device and captured its own output.
 *
 * YOU MUST MUTE THE RDP CLIENT
 *   Otherwise mstsc plays the same audio over the slow path at the same time and
 *   you hear it twice, slightly apart. Mute mstsc.exe per-app in the console
 *   session's volume mixer, or point it at a device you don't listen to.
 *
 * BUILD: compiled as C (COBJMACROS). Links ole32.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../common/hydra_ipc.h"

/* Own GUID constants -- DEFINE_GUID/INITGUID differ between MSVC and MinGW for
 * the WASAPI interfaces and no import lib carries them reliably. Same pattern as
 * the other audio tools here. */
static const GUID HG_CLSID_MMDeviceEnumerator =
    {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID HG_IID_IMMDeviceEnumerator =
    {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID HG_IID_IAudioClient =
    {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID HG_IID_IAudioCaptureClient =
    {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID HG_IID_IAudioRenderClient =
    {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const PROPERTYKEY HG_PKEY_Device_FriendlyName =
    {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};

static volatile BOOL g_run = TRUE;
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; g_run = FALSE; return TRUE; }

static void L(const wchar_t* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fwprintf(stderr, L"[abridge] ");
    vfwprintf(stderr, fmt, ap);
    fwprintf(stderr, L"\n");
    va_end(ap);
    fflush(stderr);
}

/* Report a retry failure, rate-limited, with a running count.
 *
 * Every failure path here used to `goto again` in silence, so a bridge that
 * could never start looked exactly like one working quietly: the process alive,
 * the log holding only its startup lines. That is the same trap the capture
 * agent fell into -- a permanent failure indistinguishable from health -- and it
 * is worth an extra line of log to never repeat it.
 *
 * Rate-limited to one line per ~10 attempts so a long outage does not fill the
 * log, while still proving something is wrong. */
static void retry_note(const wchar_t* what, HRESULT hr, int* attempt)
{
    if ((*attempt % 10) == 0)
        L(L"%ls failed (hr=0x%08lX) -- retry %d; will keep trying",
          what, (unsigned long)hr, *attempt);
    (*attempt)++;
}

static int wcontains_ci(const wchar_t* hay, const wchar_t* needle)
{
    if (!hay || !needle || !*needle) return 0;
    size_t nl = wcslen(needle);
    for (const wchar_t* p = hay; *p; ++p)
        if (_wcsnicmp(p, needle, nl) == 0) return 1;
    return 0;
}

/* Open the shared ring. hydrad creates it -- creating a Global\ object needs a
 * privilege the seat user doesn't have, the same constraint that applies to the
 * video transport. */
static HydraAudioRing* open_ring(const char* seat, float** samples, HANDLE* mapOut)
{
    wchar_t name[128];
    hydra_audio_name(name, 128, seat);
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return NULL;
    void* v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!v) { CloseHandle(m); return NULL; }
    *mapOut  = m;
    *samples = (float*)((BYTE*)v + sizeof(HydraAudioRing));
    return (HydraAudioRing*)v;
}

static IMMDevice* find_render_ep(IMMDeviceEnumerator* en, const wchar_t* match)
{
    IMMDevice* found = NULL;
    if (!match) {
        IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &found);
    } else {
        IMMDeviceCollection* col = NULL;
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)))
            return NULL;
        UINT n = 0; IMMDeviceCollection_GetCount(col, &n);
        for (UINT i = 0; i < n && !found; ++i) {
            IMMDevice* d = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            LPWSTR id = NULL; IMMDevice_GetId(d, &id);
            if (id && wcontains_ci(id, match)) { IMMDevice_AddRef(d); found = d; }
            if (id) CoTaskMemFree(id);
            IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
    }
    if (found) {
        IPropertyStore* ps = NULL; wchar_t nm[256] = L"(unknown)";
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(found, STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &HG_PKEY_Device_FriendlyName, &v))
                && v.vt == VT_LPWSTR)
                wcsncpy_s(nm, 256, v.pwszVal, _TRUNCATE);
            PropVariantClear(&v);
            IPropertyStore_Release(ps);
        }
        L(L"endpoint: %ls", nm);
    }
    return found;
}

/* --------------------------------------------------------------------------
 * CAPTURE -- in the seat's session
 * ------------------------------------------------------------------------ */
static int run_capture(const char* seat)
{
    HANDLE map = NULL; float* ring = NULL;
    HydraAudioRing* hdr = NULL;

    while (g_run && !hdr) {
        hdr = open_ring(seat, &ring, &map);
        if (!hdr) { L(L"waiting for the shared ring (is the service up?)"); Sleep(2000); }
    }
    if (!hdr) return 1;

    hdr->rate = HYDRA_AUD_RATE; hdr->channels = HYDRA_AUD_CHANNELS;
    hdr->ringFrames = HYDRA_AUD_FRAMES;

    int attempt = 0;
    while (g_run) {
        IMMDeviceEnumerator* en = NULL; IMMDevice* dev = NULL;
        IAudioClient* cli = NULL; IAudioCaptureClient* cap = NULL;
        WAVEFORMATEX* fmt = NULL;
        HRESULT hr;

        hr = CoCreateInstance(&HG_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                              &HG_IID_IMMDeviceEnumerator, (void**)&en);
        if (FAILED(hr)) { retry_note(L"MMDeviceEnumerator", hr, &attempt); goto again; }

        /* The session's default endpoint. Inside an RDP session that is
         * "Remote Audio" -- virtual, so loopback-recording it cannot feed back. */
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
        if (FAILED(hr)) {
            retry_note(L"no default render endpoint in this session "
                       L"(is the RDP session connected?)", hr, &attempt);
            goto again;
        }
        hr = IMMDevice_Activate(dev, &HG_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&cli);
        if (FAILED(hr)) { retry_note(L"endpoint Activate", hr, &attempt); goto again; }
        hr = IAudioClient_GetMixFormat(cli, &fmt);
        if (FAILED(hr)) { retry_note(L"GetMixFormat", hr, &attempt); goto again; }

        /* Take whatever rate the session's endpoint runs at and record it in the
         * ring header; the renderer resamples. Insisting on a fixed rate was
         * wrong -- the seat's "Remote Audio" endpoint is virtual and exposes no
         * Advanced properties, so its format (44100 Hz here) cannot be changed.
         * Channel count we do require, because mixing down is a different job. */
        if (fmt->nChannels != HYDRA_AUD_CHANNELS) {
            L(L"session mix is %uch; this bridge carries stereo only", fmt->nChannels);
            goto again;
        }
        hdr->rate = fmt->nSamplesPerSec;

        hr = IAudioClient_Initialize(cli, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     2000000, 0, fmt, NULL);
        if (FAILED(hr)) { retry_note(L"loopback Initialize", hr, &attempt); goto again; }
        hr = IAudioClient_GetService(cli, &HG_IID_IAudioCaptureClient, (void**)&cap);
        if (FAILED(hr)) { retry_note(L"GetService(capture)", hr, &attempt); goto again; }
        hr = IAudioClient_Start(cli);
        if (FAILED(hr)) { retry_note(L"capture Start", hr, &attempt); goto again; }

        attempt = 0;      /* recovered */
        hdr->running = 1;
        L(L"capturing session mix: %luHz/%uch", fmt->nSamplesPerSec, fmt->nChannels);

        while (g_run) {
            UINT32 packet = 0;
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &packet))) break;
            if (packet == 0) { Sleep(2); continue; }

            while (packet > 0) {
                BYTE* data = NULL; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, NULL, NULL)))
                    goto lost;

                uint64_t w = hdr->writePos;
                for (UINT32 i = 0; i < frames; ++i) {
                    size_t slot = (size_t)((w + i) % HYDRA_AUD_FRAMES) * HYDRA_AUD_CHANNELS;
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        ring[slot] = 0.f; ring[slot + 1] = 0.f;
                    } else {
                        const float* in = (const float*)data + (size_t)i * HYDRA_AUD_CHANNELS;
                        ring[slot] = in[0]; ring[slot + 1] = in[1];
                    }
                }
                MemoryBarrier();
                hdr->writePos = w + frames;   /* publish only after the data is in */

                IAudioCaptureClient_ReleaseBuffer(cap, frames);
                if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &packet))) goto lost;
            }
        }
    lost:
        hdr->running = 0;
    again:
        if (cap) IAudioCaptureClient_Release(cap);
        if (cli) { IAudioClient_Stop(cli); IAudioClient_Release(cli); }
        if (fmt) CoTaskMemFree(fmt);
        if (dev) IMMDevice_Release(dev);
        if (en)  IMMDeviceEnumerator_Release(en);
        if (g_run) Sleep(1000);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * RENDER -- in the console session
 * ------------------------------------------------------------------------ */
static int run_render(const char* seat, const wchar_t* epMatch)
{
    HANDLE map = NULL; float* ring = NULL;
    HydraAudioRing* hdr = NULL;

    while (g_run && !hdr) {
        hdr = open_ring(seat, &ring, &map);
        if (!hdr) { L(L"waiting for the shared ring"); Sleep(2000); }
    }
    if (!hdr) return 1;

    /* Fractional read position, in SOURCE frames. The ring may be at a
     * different rate than the endpoint (44100 vs 48000 is the normal case here),
     * so we interpolate rather than step whole frames. */
    double  readPos = 0.0;
    int     primed  = 0;

    int attempt = 0;
    while (g_run) {
        IMMDeviceEnumerator* en = NULL; IMMDevice* dev = NULL;
        IAudioClient* cli = NULL; IAudioRenderClient* ren = NULL;
        WAVEFORMATEX* fmt = NULL;
        UINT32 bufFrames = 0;
        HRESULT hr;

        hr = CoCreateInstance(&HG_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                              &HG_IID_IMMDeviceEnumerator, (void**)&en);
        if (FAILED(hr)) { retry_note(L"MMDeviceEnumerator", hr, &attempt); goto again; }

        dev = find_render_ep(en, epMatch);
        if (!dev) {
            if ((attempt % 10) == 0)
                L(L"no render endpoint matching \"%ls\" -- retry %d. Check the id with: "
                  L"route_endpoint.exe --list", epMatch ? epMatch : L"(default)", attempt);
            attempt++;
            goto again;
        }
        hr = IMMDevice_Activate(dev, &HG_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&cli);
        if (FAILED(hr)) { retry_note(L"endpoint Activate", hr, &attempt); goto again; }
        hr = IAudioClient_GetMixFormat(cli, &fmt);
        if (FAILED(hr)) { retry_note(L"GetMixFormat", hr, &attempt); goto again; }

        /* Any rate is fine -- we resample. Stereo is required. */
        if (fmt->nChannels != HYDRA_AUD_CHANNELS) {
            L(L"monitor endpoint is %uch; this bridge carries stereo only", fmt->nChannels);
            goto again;
        }

        /* Small buffer: this is the latency we are trying to keep down. */
        hr = IAudioClient_Initialize(cli, AUDCLNT_SHAREMODE_SHARED, 0,
                                     (REFERENCE_TIME)HYDRA_AUD_TARGET_MS * 10000 * 2,
                                     0, fmt, NULL);
        if (FAILED(hr)) { retry_note(L"render Initialize", hr, &attempt); goto again; }
        hr = IAudioClient_GetService(cli, &HG_IID_IAudioRenderClient, (void**)&ren);
        if (FAILED(hr)) { retry_note(L"GetService(render)", hr, &attempt); goto again; }
        IAudioClient_GetBufferSize(cli, &bufFrames);
        hr = IAudioClient_Start(cli);
        if (FAILED(hr)) { retry_note(L"render Start", hr, &attempt); goto again; }
        attempt = 0;      /* recovered */

        L(L"rendering: ring %uHz -> endpoint %luHz/%uch, %u-frame buffer, %ums behind",
          hdr->rate ? hdr->rate : HYDRA_AUD_RATE,
          fmt->nSamplesPerSec, fmt->nChannels, bufFrames, HYDRA_AUD_TARGET_MS);
        primed = 0;

        while (g_run) {
            uint64_t w = hdr->writePos;
            MemoryBarrier();

            /* The producer flags itself; without it there is nothing to play and
             * "no sound" would otherwise be indistinguishable from silence. */
            {
                static DWORD lastWarn = 0;
                DWORD nowMs = GetTickCount();
                if (!hdr->running && (nowMs - lastWarn) > 15000) {
                    lastWarn = nowMs;
                    L(L"capture side is not running -- no audio to play "
                      L"(is abcap:<seat> up in the seat's session?)");
                }
            }

            uint32_t srcRate = hdr->rate ? hdr->rate : HYDRA_AUD_RATE;
            /* Source frames consumed per output frame. 44100/48000 = 0.91875 --
             * i.e. we read slightly slower than we write, which is exactly what
             * makes 44.1k play correctly on a 48k endpoint. */
            const double step = (double)srcRate / (double)fmt->nSamplesPerSec;

            /* Start (or resync) a fixed distance behind the write head. Too close
             * and normal jitter underruns; too far and we hand back the latency
             * this exists to remove. Also the recovery path when the reader has
             * fallen more than a ring behind -- one glitch beats permanent drift. */
            const double target = (double)srcRate * HYDRA_AUD_TARGET_MS / 1000.0;
            if (!primed || (double)w < readPos ||
                ((double)w - readPos) > (double)HYDRA_AUD_FRAMES) {
                readPos = ((double)w > target) ? ((double)w - target) : 0.0;
                primed = 1;
            }

            UINT32 pad = 0;
            if (FAILED(IAudioClient_GetCurrentPadding(cli, &pad))) break;
            UINT32 space = (bufFrames > pad) ? (bufFrames - pad) : 0;

            /* How many OUTPUT frames the source data can supply. Leave one source
             * frame spare so interpolation always has its right-hand sample. */
            double availSrc = (double)w - readPos - 1.0;
            UINT32 todo = 0;
            if (availSrc > 0.0) {
                double outFrames = availSrc / step;
                todo = (UINT32)((outFrames < (double)space) ? outFrames : (double)space);
            }
            if (todo == 0) { Sleep(2); continue; }

            BYTE* out = NULL;
            if (FAILED(IAudioRenderClient_GetBuffer(ren, todo, &out))) break;
            float* o = (float*)out;
            double pos = readPos;
            for (UINT32 i = 0; i < todo; ++i) {
                uint64_t i0 = (uint64_t)pos;
                double   fr = pos - (double)i0;
                size_t a = (size_t)(i0 % HYDRA_AUD_FRAMES) * HYDRA_AUD_CHANNELS;
                size_t b = (size_t)((i0 + 1) % HYDRA_AUD_FRAMES) * HYDRA_AUD_CHANNELS;
                /* Linear interpolation. Not audiophile-grade, but the artefacts
                 * are far above anything a classroom monitor resolves, and it
                 * costs two multiplies per sample. */
                o[(size_t)i * 2]     = (float)(ring[a]     + (ring[b]     - ring[a])     * fr);
                o[(size_t)i * 2 + 1] = (float)(ring[a + 1] + (ring[b + 1] - ring[a + 1]) * fr);
                pos += step;
            }
            IAudioRenderClient_ReleaseBuffer(ren, todo, 0);
            readPos = pos;
        }

    again:
        if (ren) IAudioRenderClient_Release(ren);
        if (cli) { IAudioClient_Stop(cli); IAudioClient_Release(cli); }
        if (fmt) CoTaskMemFree(fmt);
        if (dev) IMMDevice_Release(dev);
        if (en)  IMMDeviceEnumerator_Release(en);
        if (g_run) Sleep(1000);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        fwprintf(stderr,
            L"usage: audio_bridge capture <seat>\n"
            L"       audio_bridge render  <seat> <endpoint-substr>\n");
        return 2;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    char seat[64] = {0};
    for (int i = 0; argv[2][i] && i < 63; ++i) seat[i] = (char)argv[2][i];

    int rc;
    if (_wcsicmp(argv[1], L"capture") == 0) {
        L(L"capture mode, seat %S", seat);
        rc = run_capture(seat);
    } else if (_wcsicmp(argv[1], L"render") == 0) {
        L(L"render mode, seat %S", seat);
        rc = run_render(seat, (argc >= 4) ? argv[3] : NULL);
    } else {
        fwprintf(stderr, L"unknown mode: %ls\n", argv[1]);
        rc = 2;
    }

    CoUninitialize();
    return rc;
}
