/* audio_keepalive.c -- hold a seat's audio endpoint open, forever, with silence.
 *
 * THE PROBLEM THIS SOLVES
 *   In a seat's RDP session the Remote Audio endpoint goes bad once it has been
 *   idle: the FIRST application to open it gets silence. Open a second app and
 *   THAT works -- and then the first one starts working too. Browsers lose this
 *   race reliably (Chrome, Edge and Firefox all failed from cold); media players
 *   often win it, which disguised it as a Chrome bug for hours.
 *
 *   chrome://media-internals settled where the audio was being lost: Chrome's
 *   pipeline reported kPlaying, BUFFERING_HAVE_ENOUGH, a cleanly selected
 *   decoder and no errors at all. It was decoding opus and handing off PCM the
 *   whole time. So nothing was wrong above the render path.
 *
 * WHAT DOESN'T WORK (all measured on hardware, don't re-try these)
 *   Restarting Audiosrv. From the console session: no effect on the seat. From a
 *   SYSTEM scheduled task in session 0: no effect. From a SYSTEM token inside the
 *   seat's session: launches fine, no effect. From the console admin's ELEVATED
 *   token inside the seat's session -- the exact combination that works when
 *   typed by hand -- no effect either.
 *
 *   The reason those all fail is the same reason the manual attempts appeared to
 *   succeed: a restart leaves the endpoint IDLE, which guarantees the next opener
 *   is the unlucky first one. The manual successes had another app already open.
 *   Restarting doesn't cure the race, it just resets the clock on it.
 *
 * WHAT THIS DOES INSTEAD
 *   Opens a shared-mode render stream on the seat's default endpoint and writes
 *   silence into it forever. The endpoint is therefore never idle, so no
 *   application is ever the first opener, and the race cannot occur.
 *
 *   Silence is genuinely silent -- WASAPI shared mode mixes it with everything
 *   else, so it is inaudible and does not affect other applications' volume. Cost
 *   is one small buffer of zeroes every ~200 ms: negligible CPU, ~1 MB RSS.
 *
 *   AUDCLNT_STREAMFLAGS_NOPERSIST keeps it out of the per-app volume mixer, so it
 *   doesn't clutter the UI or acquire a stale device assignment of its own.
 *
 * DELIBERATELY NOT event-driven: a timer-paced loop with a generous buffer is
 * less to go wrong, and latency is irrelevant when the payload is zeroes.
 *
 * RECOVERY: if the endpoint disappears (session reconnect, device change) the
 * stream dies; we tear down and rebuild rather than exiting, so this rides
 * through reconnects the way session_capture does.
 *
 * BUILD:  compiled as C (COBJMACROS). Links ole32 only.
 * USAGE:  audio_keepalive.exe [render-id-substring]
 *           no argument  -> the session's DEFAULT render endpoint (normal case:
 *                           inside an RDP session that is "Remote Audio")
 *           substring    -> pin a specific endpoint instead
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

/* GUIDs as our own constants -- DEFINE_GUID/INITGUID behave differently under
 * MSVC and MinGW for the WASAPI interfaces, and no import lib carries them
 * reliably. This pattern is used throughout the audio tools here for that
 * reason; it is bulletproof and costs nothing. */
static const GUID HG_CLSID_MMDeviceEnumerator =
    {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID HG_IID_IMMDeviceEnumerator =
    {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID HG_IID_IAudioClient =
    {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID HG_IID_IAudioRenderClient =
    {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const PROPERTYKEY HG_PKEY_Device_FriendlyName =
    {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};

#ifndef AUDCLNT_STREAMFLAGS_NOPERSIST
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00080000
#endif

static volatile BOOL g_run = TRUE;
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; g_run = FALSE; return TRUE; }

static void L(const wchar_t* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fwprintf(stderr, L"[keepalive] ");
    vfwprintf(stderr, fmt, ap);
    fwprintf(stderr, L"\n");
    va_end(ap);
    fflush(stderr);
}

static int wcontains_ci(const wchar_t* hay, const wchar_t* needle)
{
    if (!hay || !needle || !*needle) return 0;
    size_t nl = wcslen(needle);
    for (const wchar_t* p = hay; *p; ++p)
        if (_wcsnicmp(p, needle, nl) == 0) return 1;
    return 0;
}

/* Resolve the endpoint: a substring match if given, otherwise this session's
 * default console render device (which inside an RDP session is Remote Audio). */
static IMMDevice* pick_endpoint(IMMDeviceEnumerator* en, const wchar_t* match)
{
    IMMDevice* found = NULL;

    if (!match) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &found)))
            return NULL;
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
        IPropertyStore* ps = NULL;
        wchar_t name[256] = L"(unknown)";
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(found, STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &HG_PKEY_Device_FriendlyName, &v))
                && v.vt == VT_LPWSTR)
                wcsncpy_s(name, 256, v.pwszVal, _TRUNCATE);
            PropVariantClear(&v);
            IPropertyStore_Release(ps);
        }
        L(L"holding open: %ls", name);
    }
    return found;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* match = (argc >= 2) ? argv[1] : NULL;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    L(L"starting%ls%ls", match ? L", endpoint ~" : L" (default endpoint)",
      match ? match : L"");

    UINT64 cycles = 0;
    DWORD lastLog = 0;

    /* Outer loop: (re)acquire the endpoint and hold it. Never exits on failure --
     * the endpoint legitimately disappears across RDP reconnects, and the whole
     * point is to be holding it open again the moment it returns. */
    while (g_run) {
        IMMDeviceEnumerator* en = NULL;
        IMMDevice*           dev = NULL;
        IAudioClient*        cli = NULL;
        IAudioRenderClient*  ren = NULL;
        WAVEFORMATEX*        fmt = NULL;
        UINT32               bufFrames = 0;
        HRESULT hr;

        if (FAILED(CoCreateInstance(&HG_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                    &HG_IID_IMMDeviceEnumerator, (void**)&en)))
            goto retry;

        dev = pick_endpoint(en, match);
        if (!dev) goto retry;

        if (FAILED(IMMDevice_Activate(dev, &HG_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&cli)))
            goto retry;
        if (FAILED(IAudioClient_GetMixFormat(cli, &fmt)))
            goto retry;

        /* 200 ms buffer: plenty of slack, so a late wakeup never underruns.
         * NOPERSIST keeps this stream out of the per-app volume mixer. */
        hr = IAudioClient_Initialize(cli, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_NOPERSIST,
                                     2000000, 0, fmt, NULL);
        if (FAILED(hr)) { L(L"Initialize failed hr=0x%08lX", (unsigned long)hr); goto retry; }

        if (FAILED(IAudioClient_GetService(cli, &HG_IID_IAudioRenderClient, (void**)&ren)))
            goto retry;
        IAudioClient_GetBufferSize(cli, &bufFrames);

        /* Pre-fill with silence so the stream is never empty at start. */
        {
            BYTE* p = NULL;
            if (SUCCEEDED(IAudioRenderClient_GetBuffer(ren, bufFrames, &p)))
                IAudioRenderClient_ReleaseBuffer(ren, bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        if (FAILED(IAudioClient_Start(cli))) goto retry;
        L(L"stream open: %luHz/%uch, %u-frame buffer -- endpoint will not go idle",
          fmt->nSamplesPerSec, fmt->nChannels, bufFrames);

        /* Inner loop: top the buffer up with silence. Using the SILENT flag means
         * we never even have to write zeroes -- WASAPI does it. */
        while (g_run) {
            UINT32 pad = 0;
            hr = IAudioClient_GetCurrentPadding(cli, &pad);
            if (FAILED(hr)) {
                L(L"padding failed hr=0x%08lX -- endpoint lost, rebuilding", (unsigned long)hr);
                break;
            }
            UINT32 avail = (bufFrames > pad) ? (bufFrames - pad) : 0;
            if (avail > 0) {
                BYTE* p = NULL;
                hr = IAudioRenderClient_GetBuffer(ren, avail, &p);
                if (FAILED(hr)) {
                    L(L"GetBuffer failed hr=0x%08lX -- endpoint lost, rebuilding", (unsigned long)hr);
                    break;
                }
                IAudioRenderClient_ReleaseBuffer(ren, avail, AUDCLNT_BUFFERFLAGS_SILENT);
                ++cycles;
            }

            DWORD now = GetTickCount();
            if (now - lastLog >= 300000) {        /* 5 min heartbeat, not chatter */
                lastLog = now;
                L(L"alive: %llu cycles", (unsigned long long)cycles);
            }
            Sleep(50);
        }

    retry:
        if (ren) IAudioRenderClient_Release(ren);
        if (cli) { IAudioClient_Stop(cli); IAudioClient_Release(cli); }
        if (fmt) CoTaskMemFree(fmt);
        if (dev) IMMDevice_Release(dev);
        if (en)  IMMDeviceEnumerator_Release(en);
        if (g_run) Sleep(2000);                   /* endpoint not there yet; wait */
    }

    L(L"stopping");
    CoUninitialize();
    return 0;
}
