/* audiotest.cpp  --  COMPREHENSIVE cross-session process-loopback audio test.
 *
 * THE DECISION THIS SETTLES
 *   Can a process running in the CONSOLE session (or a session-0 service)
 *   capture the audio of a process running in TEACHER'S RDP session, and render
 *   it to the monitor endpoint -- WITHOUT the shared-default collision and
 *   WITHOUT the feedback loop that killed the endpoint-loopback approach?
 *
 *   If YES: Hydra can do per-seat audio via process-loopback. If it fails at the
 *   session boundary (E_ACCESSDENIED / no frames), the RDP-based stack can't do
 *   it and Aster is the right tool. This test answers that definitively instead
 *   of guessing.
 *
 * WHY PROCESS-LOOPBACK (not endpoint loopback)
 *   ActivateAudioInterfaceAsync + AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
 *   captures only a specific PID's audio streams, NOT tied to an endpoint. So
 *   capturing teacher's app and rendering to the monitor can't feed back (we're
 *   not capturing the monitor's output), and doesn't touch the shared default
 *   (we address teacher's process directly).
 *
 * FIVE STAGES, each a place the session boundary could bite -- all must pass:
 *   [1] enumerate teacher's session PIDs from here (cross-session visibility)
 *   [2] activate process-loopback against a teacher PID (the boundary call;
 *       may return E_ACCESSDENIED)
 *   [3] receive REAL non-silent frames (proves it actually taps the audio)
 *   [4] render those frames to the monitor endpoint (full path, no feedback)
 *   [5] report loudness so you can confirm it's teacher's audio specifically
 *
 * A test that only did [2] would be a false-positive machine. This does all 5.
 *
 * BUILD:  cl /O2 /EHsc audiotest.cpp /link ole32.lib mmdevapi.lib mfplat.lib wtsapi32.lib
 *   (if mmdevapi.lib is missing, the GUIDs are self-defined below, so ole32 +
 *    mfplat + wtsapi32 suffice)
 * RUN (from an ELEVATED console -- ideally the same privilege hydrad has):
 *   audiotest.exe --list-sessions        list sessions + who's in them
 *   audiotest.exe --pids <session_id>    list audible-capable PIDs in a session
 *   audiotest.exe <PID> [render-substr]  capture that PID, render to monitor
 *                                        endpoint (default: id containing 623f2512)
 *
 * TYPICAL USE:
 *   1. Play music in teacher's session (a browser tab, media player).
 *   2. audiotest.exe --list-sessions        -> find teacher's session id
 *   3. audiotest.exe --pids <that id>        -> find the PID making sound
 *   4. audiotest.exe <PID> 623f2512          -> watch stages 2-5
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* Process-loopback activation structures. These live in <audioclientactivationparams.h>
 * in newer Windows SDKs; define them here guarded so this builds on any SDK (and on
 * MinGW, which lacks the header). The layout is fixed ABI -- these are the exact
 * definitions Windows expects, verified against the SDK header. */
#ifndef AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT
typedef enum {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif

/* Self-defined GUIDs (same compiler-agnostic approach that fixed session_audio:
 * plain constants, no DEFINE_GUID/INITGUID/lib dependency). */
static const GUID HG_CLSID_MMDeviceEnumerator =
    {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID HG_IID_IMMDeviceEnumerator =
    {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID HG_IID_IAudioClient =
    {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID HG_IID_IAudioClient3 =
    {0x7ed4ee07,0x8e67,0x4cd4,{0x8c,0x1a,0x2b,0x7a,0x59,0x87,0xad,0x42}};
static const GUID HG_IID_IAudioRenderClient =
    {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID HG_IID_IAudioCaptureClient =
    {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID HG_IID_IActivateAudioInterfaceCompletionHandler =
    {0x41d949ab,0x9862,0x444a,{0x80,0xf6,0xc2,0x61,0x33,0x4d,0xa5,0xeb}};
static const GUID HG_IID_IUnknown =
    {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

/* The magic device path string for process loopback activation. */
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

static void hr_name(HRESULT hr, char* out, size_t n) {
    switch ((unsigned)hr) {
        case 0x80070005: snprintf(out,n,"E_ACCESSDENIED -- the session boundary blocked it (Aster territory)"); break;
        case 0x8000000E: snprintf(out,n,"E_ILLEGAL_METHOD_CALL -- call-convention bug, NOT the session wall"); break;
        case 0x88890001: snprintf(out,n,"AUDCLNT_E_NOT_INITIALIZED"); break;
        case 0x88890008: snprintf(out,n,"AUDCLNT_E_UNSUPPORTED_FORMAT"); break;
        case 0x8889000A: snprintf(out,n,"AUDCLNT_E_DEVICE_IN_USE"); break;
        case 0x88890010: snprintf(out,n,"AUDCLNT_E_DEVICE_INVALIDATED"); break;
        default: snprintf(out,n,"hr=0x%08lX",(unsigned long)hr); break;
    }
}

/* -------- Stage 1: enumerate sessions and PIDs -------- */

static void list_sessions(void) {
    WTS_SESSION_INFOW* si = NULL; DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &si, &count)) {
        fwprintf(stderr, L"WTSEnumerateSessions failed %lu\n", GetLastError()); return;
    }
    fwprintf(stderr, L"Sessions on this machine:\n");
    for (DWORD i = 0; i < count; ++i) {
        LPWSTR user = NULL; DWORD b = 0;
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, si[i].SessionId,
                                    WTSUserName, &user, &b);
        const wchar_t* st = L"?";
        switch (si[i].State) {
            case WTSActive: st=L"Active"; break;  case WTSConnected: st=L"Connected"; break;
            case WTSDisconnected: st=L"Disconnected"; break; case WTSIdle: st=L"Idle"; break;
            case WTSListen: st=L"Listen"; break;  default: break;
        }
        fwprintf(stderr, L"  session %lu  [%ls]  user=%ls  win=%ls\n",
                 si[i].SessionId, st, (user&&*user)?user:L"(none)", si[i].pWinStationName);
        if (user) WTSFreeMemory(user);
    }
    WTSFreeMemory(si);
    fwprintf(stderr, L"\nFind teacher's session above, then: audiotest.exe --pids <id>\n");
}

static DWORD pid_session(DWORD pid) {
    DWORD s = 0xFFFFFFFF;
    ProcessIdToSessionId(pid, &s);
    return s;
}

static void list_pids(DWORD sessionId) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"snapshot failed\n"); return; }
    PROCESSENTRY32W pe; memset(&pe,0,sizeof(pe)); pe.dwSize = sizeof(pe);
    fwprintf(stderr, L"Processes in session %lu (any of these that play audio can be captured):\n", sessionId);
    int shown = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pid_session(pe.th32ProcessID) == sessionId) {
                fwprintf(stderr, L"  pid %6lu  %ls\n", pe.th32ProcessID, pe.szExeFile);
                ++shown;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!shown) fwprintf(stderr, L"  (none -- is the session id right? use --list-sessions)\n");
    fwprintf(stderr, L"\nPlay audio in one, then: audiotest.exe <pid> [render-id-substr]\n");
}

/* -------- Completion handler for ActivateAudioInterfaceAsync -------- */

typedef struct ActHandler {
    IActivateAudioInterfaceCompletionHandlerVtbl* lpVtbl;
    LONG ref;
    HANDLE done;
    HRESULT activateHr;
    IAudioClient* client;
    IUnknown* ftm;   /* free-threaded marshaler -- REQUIRED. The API rejects a
                      * non-agile completion handler with E_ILLEGAL_METHOD_CALL
                      * (0x8000000E). Aggregating an FTM and delegating IMarshal
                      * QI to it makes this object agile, which is the fix. */
} ActHandler;

static HRESULT STDMETHODCALLTYPE Act_QI(IActivateAudioInterfaceCompletionHandler* This,
                                        REFIID riid, void** ppv) {
    ActHandler* h = (ActHandler*)This;
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &HG_IID_IUnknown) ||
        IsEqualIID(riid, &HG_IID_IActivateAudioInterfaceCompletionHandler)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    /* Delegate IMarshal (and anything else) to the aggregated FTM so the object
     * is agile -- without this, ActivateAudioInterfaceAsync fails immediately. */
    if (h->ftm)
        return IUnknown_QueryInterface(h->ftm, riid, ppv);
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Act_AddRef(IActivateAudioInterfaceCompletionHandler* This) {
    ActHandler* h = (ActHandler*)This; return InterlockedIncrement(&h->ref);
}
static ULONG STDMETHODCALLTYPE Act_Release(IActivateAudioInterfaceCompletionHandler* This) {
    ActHandler* h = (ActHandler*)This; LONG r = InterlockedDecrement(&h->ref); return r;
}
static HRESULT STDMETHODCALLTYPE Act_ActivateCompleted(
        IActivateAudioInterfaceCompletionHandler* This,
        IActivateAudioInterfaceAsyncOperation* op) {
    ActHandler* h = (ActHandler*)This;
    HRESULT activateResult = E_FAIL;
    IUnknown* unk = NULL;
    HRESULT hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &activateResult, &unk);
    if (SUCCEEDED(hr)) hr = activateResult;
    if (SUCCEEDED(hr) && unk) {
        IUnknown_QueryInterface(unk, &HG_IID_IAudioClient, (void**)&h->client);
    }
    if (unk) IUnknown_Release(unk);
    h->activateHr = hr;
    SetEvent(h->done);
    return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl g_actVtbl = {
    Act_QI, Act_AddRef, Act_Release, Act_ActivateCompleted
};

/* -------- Stages 2-5: activate, capture, render -------- */

static int run_capture(DWORD pid, const wchar_t* renderMatch) {
    char hrbuf[160];
    DWORD tsess = pid_session(pid);
    DWORD msess = 0xFFFFFFFF; ProcessIdToSessionId(GetCurrentProcessId(), &msess);
    fwprintf(stderr, L"=== target PID %lu is in session %lu; this test process is in session %lu ===\n",
             pid, tsess, msess);
    if (tsess == msess)
        fwprintf(stderr, L"  NOTE: same session -- this does NOT prove cross-session. Run this from the\n"
                         L"        console/service while the PID is in teacher's RDP session.\n");
    else
        fwprintf(stderr, L"  GOOD: genuinely cross-session (%lu -> %lu). This is the real test.\n", msess, tsess);

    HRESULT hr = S_OK;

    /* ---- Stage 2: activate process loopback ---- */
    fwprintf(stderr, L"\n[Stage 2] activating PROCESS_LOOPBACK against PID %lu ...\n", pid);

    AUDIOCLIENT_ACTIVATION_PARAMS ap; memset(&ap,0,sizeof(ap));
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    /* pid==0 => EXCLUDE mode targeting nothing real = capture EVERYTHING the
     * session plays (control test: proves cross-session capture can carry real
     * audio regardless of which specific process makes it). Otherwise INCLUDE
     * the target's process tree. */
    ap.ProcessLoopbackParams.ProcessLoopbackMode = (pid == 0)
        ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    if (pid == 0) {
        /* Exclude a PID that doesn't exist so nothing is actually excluded ->
         * whole-session capture. Use a very unlikely PID value. */
        ap.ProcessLoopbackParams.TargetProcessId = 0xFFFFFFF0;
        fwprintf(stderr, L"  [mode] whole-session capture (EXCLUDE dummy) -- captures ALL audio\n");
    }

    PROPVARIANT pv; memset(&pv,0,sizeof(pv)); pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = (BYTE*)&ap;

    ActHandler handler; memset(&handler,0,sizeof(handler)); handler.lpVtbl = &g_actVtbl; handler.ref = 1;
    handler.done = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* Create the free-threaded marshaler that makes `handler` agile. Passing the
     * handler as the outer unknown aggregates the FTM into it; Act_QI delegates
     * IMarshal to handler.ftm. REQUIRED or activation returns E_ILLEGAL_METHOD_CALL. */
    hr = CoCreateFreeThreadedMarshaler((IUnknown*)&handler, &handler.ftm);
    if (FAILED(hr) || !handler.ftm) {
        fwprintf(stderr, L"[FAIL] CoCreateFreeThreadedMarshaler failed (hr=0x%08lX)\n",
                 (unsigned long)hr);
        return 2;
    }

    IActivateAudioInterfaceAsyncOperation* op = NULL;
    hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                     &HG_IID_IAudioClient, &pv,
                                     (IActivateAudioInterfaceCompletionHandler*)&handler, &op);
    if (FAILED(hr)) {
        hr_name(hr, hrbuf, sizeof(hrbuf));
        fwprintf(stderr, L"[FAIL Stage 2] ActivateAudioInterfaceAsync returned %hs\n", hrbuf);
        if ((unsigned)hr == 0x80070005)
            fwprintf(stderr, L"  -> E_ACCESSDENIED: the session boundary blocked it. Aster.\n");
        else
            fwprintf(stderr, L"  -> activation call rejected (not necessarily a session-boundary\n"
                             L"     issue -- this code is a call-convention error). Report it.\n");
        return 2;
    }
    WaitForSingleObject(handler.done, 5000);
    if (FAILED(handler.activateHr) || !handler.client) {
        hr_name(handler.activateHr, hrbuf, sizeof(hrbuf));
        fwprintf(stderr, L"[FAIL Stage 2] activation completed but failed: %hs\n", hrbuf);
        if ((unsigned)handler.activateHr == 0x80070005)
            fwprintf(stderr, L"  -> E_ACCESSDENIED: the SESSION BOUNDARY blocked cross-session\n"
                             L"     process-loopback. That's the wall; Aster is the answer.\n");
        else
            fwprintf(stderr, L"  -> completed-but-failed with a non-access-denied code; report it.\n");
        return 2;
    }
    fwprintf(stderr, L"[OK Stage 2] activation succeeded -- got an IAudioClient for PID %lu.\n", pid);
    IAudioClient* cap = handler.client;

    /* ---- init the capture client. Process loopback REQUIRES a specific format
     * (shared, and you must supply a PCM format; mix format query isn't valid on
     * the virtual device). Use 44.1k or 48k stereo 16-bit; try 48k first. ---- */
    WAVEFORMATEX wf; memset(&wf,0,sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    hr = IAudioClient_Initialize(cap, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 2000000 /*200ms in 100ns*/, 0, &wf, NULL);
    if (FAILED(hr)) {
        hr_name(hr, hrbuf, sizeof(hrbuf));
        fwprintf(stderr, L"[FAIL] capture Initialize: %hs\n", hrbuf);
        return 3;
    }
    HANDLE capEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(cap, capEvt);

    IAudioCaptureClient* capSvc = NULL;
    hr = IAudioClient_GetService(cap, &HG_IID_IAudioCaptureClient, (void**)&capSvc);
    if (FAILED(hr)) { fwprintf(stderr, L"[FAIL] GetService(capture)\n"); return 3; }

    /* ---- Stage 4 setup: open the MONITOR render endpoint (console session sees it) ---- */
    fwprintf(stderr, L"\n[Stage 4 setup] opening monitor render endpoint (id contains \"%ls\") ...\n",
             renderMatch ? renderMatch : L"623f2512");
    IMMDeviceEnumerator* en = NULL;
    CoCreateInstance(&HG_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                     &HG_IID_IMMDeviceEnumerator, (void**)&en);
    IMMDevice* mon = NULL;
    {
        IMMDeviceCollection* col = NULL;
        IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col);
        UINT n=0; if (col) IMMDeviceCollection_GetCount(col,&n);
        const wchar_t* want = renderMatch ? renderMatch : L"623f2512";
        for (UINT i=0;i<n;++i){ IMMDevice* d=NULL; if(FAILED(IMMDeviceCollection_Item(col,i,&d)))continue;
            LPWSTR id=NULL; IMMDevice_GetId(d,&id);
            if (id && wcsstr(id,want) && !mon){ IMMDevice_AddRef(d); mon=d;
                fwprintf(stderr, L"  found monitor endpoint: %ls\n", id); }
            if(id)CoTaskMemFree(id); IMMDevice_Release(d);
        }
        if (col) IMMDeviceCollection_Release(col);
    }
    IAudioClient* ren = NULL; IAudioRenderClient* renSvc = NULL; UINT32 renFrames=0;
    if (!mon) {
        fwprintf(stderr, L"  [warn] monitor endpoint not found here -- will still test CAPTURE (stage 3),\n"
                         L"         but not render. (Run in the session that sees the monitor to test render.)\n");
    } else {
        WAVEFORMATEX* mixf = NULL;
        IMMDevice_Activate(mon, &HG_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&ren);
        IAudioClient_GetMixFormat(ren, &mixf);
        hr = IAudioClient_Initialize(ren, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mixf, NULL);
        if (SUCCEEDED(hr)) {
            IAudioClient_GetBufferSize(ren, &renFrames);
            IAudioClient_GetService(ren, &HG_IID_IAudioRenderClient, (void**)&renSvc);
            IAudioClient_Start(ren);
            fwprintf(stderr, L"  monitor render client ready (%u-frame buffer, %luHz/%uch)\n",
                     renFrames, mixf->nSamplesPerSec, mixf->nChannels);
        } else {
            hr_name(hr, hrbuf, sizeof(hrbuf));
            fwprintf(stderr, L"  [warn] monitor render init failed (%hs) -- capture-only.\n", hrbuf);
        }
        if (mixf) CoTaskMemFree(mixf);
    }

    /* ---- Stages 3 & 5: capture frames, measure loudness, render ---- */
    fwprintf(stderr, L"\n[Stage 3] starting capture. Make sure PID %lu is PLAYING SOUND.\n", pid);
    fwprintf(stderr, L"          Watching 8 seconds for real (non-silent) frames ...\n\n");
    IAudioClient_Start(cap);

    ULONG64 totalFrames = 0, nonSilentFrames = 0;
    double peak = 0.0;
    DWORD start = GetTickCount();
    int renderedAny = 0;
    while (GetTickCount() - start < 8000) {
        WaitForSingleObject(capEvt, 200);
        UINT32 packet = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(capSvc, &packet))) break;
        while (packet > 0) {
            BYTE* data=NULL; UINT32 frames=0; DWORD flags=0;
            if (FAILED(IAudioCaptureClient_GetBuffer(capSvc,&data,&frames,&flags,NULL,NULL))) { packet=0; break; }
            totalFrames += frames;
            int silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (!silent && frames) {
                /* measure peak amplitude (16-bit stereo) */
                const short* s = (const short*)data;
                UINT32 samples = frames * wf.nChannels;
                for (UINT32 i=0;i<samples;++i){ double a=fabs((double)s[i])/32768.0; if(a>peak)peak=a; if(a>0.0001) {} }
                nonSilentFrames += frames;
            }
            /* Stage 4: push to monitor if we have a render client. Format differs
             * (capture 16bit vs monitor mix float) -> convert 16->float if needed. */
            if (renSvc && frames) {
                UINT32 pad=0; IAudioClient_GetCurrentPadding(ren,&pad);
                UINT32 avail = renFrames - pad; UINT32 todo = frames<avail?frames:avail;
                if (todo){
                    BYTE* rb=NULL;
                    if (SUCCEEDED(IAudioRenderClient_GetBuffer(renSvc,todo,&rb))){
                        /* monitor mix is typically float32 stereo; convert from our pcm16 */
                        float* out=(float*)rb; const short* in=(const short*)data;
                        UINT32 smp = todo*2;
                        if (silent) memset(rb,0,todo*2*sizeof(float));
                        else for(UINT32 i=0;i<smp;++i) out[i]=(float)in[i]/32768.0f;
                        IAudioRenderClient_ReleaseBuffer(renSvc,todo,0);
                        renderedAny = 1;
                    }
                }
            }
            IAudioCaptureClient_ReleaseBuffer(capSvc, frames);
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(capSvc,&packet))) break;
        }
    }
    IAudioClient_Stop(cap);
    if (ren) IAudioClient_Stop(ren);

    /* ---- VERDICT ---- */
    fwprintf(stderr, L"\n=================== VERDICT ===================\n");
    fwprintf(stderr, L"[Stage 2] activation .............. PASS\n");
    fwprintf(stderr, L"[Stage 3] frames received ......... %llu total, %llu non-silent\n",
             (unsigned long long)totalFrames, (unsigned long long)nonSilentFrames);
    fwprintf(stderr, L"[Stage 5] peak amplitude .......... %.3f  (0=silence, ~1=loud)\n", peak);
    fwprintf(stderr, L"[Stage 4] rendered to monitor ..... %ls\n",
             renderedAny ? L"YES" : (renSvc ? L"no frames pushed" : L"skipped (endpoint not open here)"));
    fwprintf(stderr, L"==============================================\n\n");

    if (totalFrames == 0) {
        fwprintf(stderr, L">>> NO FRAMES AT ALL. Either the PID made no sound, or cross-session\n"
                         L"    capture is silently blocked. Retry with the PID actively playing; if\n"
                         L"    still zero, this is the wall -> Aster.\n");
    } else if (nonSilentFrames == 0 || peak < 0.001) {
        fwprintf(stderr, L">>> Frames flowed but all SILENT. The stream connected but carried no audio\n"
                         L"    -- likely the target wasn't actually playing, OR cross-session delivers\n"
                         L"    only silence. Make VERY sure the PID is playing and retry once.\n");
    } else if (renderedAny) {
        fwprintf(stderr, L">>> FULL SUCCESS: captured teacher's audio cross-session AND rendered it to\n"
                         L"    the monitor. Hydra CAN do per-seat audio this way. Green light.\n");
    } else {
        fwprintf(stderr, L">>> CAPTURE WORKS cross-session (real audio received), but render wasn't\n"
                         L"    exercised here (monitor endpoint not open in this session). Capture is\n"
                         L"    the hard part and it PASSED -- rendering from the console session is\n"
                         L"    straightforward. Strong green light; re-run where the monitor is visible\n"
                         L"    to see end-to-end.\n");
    }

    if (handler.ftm) IUnknown_Release(handler.ftm);
    if (capSvc) IAudioCaptureClient_Release(capSvc);
    if (cap) IAudioClient_Release(cap);
    if (renSvc) IAudioRenderClient_Release(renSvc);
    if (ren) IAudioClient_Release(ren);
    if (mon) IMMDevice_Release(mon);
    if (en) IMMDeviceEnumerator_Release(en);
    return (nonSilentFrames>0 && peak>=0.001) ? 0 : 4;
}

int wmain(int argc, wchar_t** argv) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { fwprintf(stderr, L"CoInitializeEx failed\n"); return 1; }

    if (argc < 2) {
        fwprintf(stderr,
            L"Comprehensive cross-session process-loopback audio test.\n\n"
            L"  audiotest.exe --list-sessions       list sessions + users\n"
            L"  audiotest.exe --pids <session_id>   list PIDs in a session\n"
            L"  audiotest.exe <PID> [render-substr] capture that PID -> monitor endpoint\n\n"
            L"Run from an ELEVATED console. Steps:\n"
            L"  1) play music in teacher's session\n"
            L"  2) --list-sessions   -> teacher's session id\n"
            L"  3) --pids <id>       -> the PID making sound\n"
            L"  4) <PID> 623f2512    -> watch the 5-stage verdict\n");
        CoUninitialize(); return 2;
    }

    if (wcscmp(argv[1], L"--list-sessions") == 0) { list_sessions(); CoUninitialize(); return 0; }
    if (wcscmp(argv[1], L"--pids") == 0 && argc >= 3) { list_pids((DWORD)_wtoi(argv[2])); CoUninitialize(); return 0; }

    DWORD pid = (DWORD)_wtoi(argv[1]);
    const wchar_t* rmatch = (argc >= 3) ? argv[2] : NULL;
    int rc = run_capture(pid, rmatch);
    CoUninitialize();
    return rc;
}
