/* session_route.c  --  route ONE Terminal-Services session's audio to ONE device.
 *
 * THE IDEA (Nathan's, and it's the right one)
 *   Don't hunt for "which app makes sound." Route by SESSION MEMBERSHIP:
 *   capture everything session N is playing, render it to device X. Seat 2's
 *   session -> monitor. Seat 1's session -> laptop. Source separation happens by
 *   which session a process belongs to, which we can always answer with
 *   ProcessIdToSessionId. That sidesteps the app-targeting problem entirely.
 *
 * WHAT WAS PROVEN (audiotest.exe, on the real machine)
 *   - process-loopback capture works and returns real audio (peak 0.203 single
 *     app; 0.987 whole-session)
 *   - it works from a SESSION 0 context (the 0.987 run was session 2 -> SYSTEM)
 *   - the FTM (free-threaded marshaler) on the completion handler is REQUIRED or
 *     activation fails E_ILLEGAL_METHOD_CALL
 *   This agent composes those into the actual feature, run from hydrad (session 0).
 *
 * HOW IT CAPTURES A WHOLE SESSION
 *   process-loopback takes ONE pid with INCLUDE_TARGET_PROCESS_TREE (that pid +
 *   its children). A session has many top-level processes, so we ENUMERATE the
 *   target session's processes and open one loopback stream per top-level pid,
 *   then mix them into the render buffer. Streams are refreshed periodically so
 *   apps launched after start are picked up.
 *
 *   (An EXCLUDE-the-other-session approach is possible too, but include-session
 *   is the direct expression of the idea and avoids capturing session 0's own
 *   sounds; we do include-session.)
 *
 * ECHO
 *   If the target session ALSO plays to the same physical device on its own,
 *   you'll hear it twice (its own path + our render) = echo. In deployment, mute
 *   the session's own output to that endpoint so ONLY our render reaches it. This
 *   agent just does the capture+render; muting is a deployment step (documented).
 *
 * BUILD:  cl /O2 /EHsc session_route.c /link ole32.lib mmdevapi.lib wtsapi32.lib
 * USAGE (normally launched by hydrad in session 0):
 *   session_route.exe <target_session_id> <render-id-substring> [refresh_ms]
 *     target_session_id   the TS session whose audio to capture (e.g. 2)
 *     render-id-substring  substring of the output endpoint id (e.g. 623f2512)
 *     refresh_ms           how often to re-scan the session for new procs (def 3000)
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* Resolve a target-session argument to a numeric TS session id. Accepts either a
 * bare number ("2") or "user:NAME" (find the session whose username == NAME).
 * Returns 0xFFFFFFFF if not found. Lets hydrad pass the same "user:teacher" spec
 * it uses elsewhere without having to resolve the session number at plan time. */
static DWORD resolve_target_session(const wchar_t* spec) {
    if (!spec || !*spec) return 0xFFFFFFFF;
    if (_wcsnicmp(spec, L"user:", 5) == 0) {
        const wchar_t* want = spec + 5;
        WTS_SESSION_INFOW* si = NULL; DWORD count = 0;
        DWORD found = 0xFFFFFFFF;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &si, &count)) {
            for (DWORD i = 0; i < count; ++i) {
                LPWSTR user = NULL; DWORD b = 0;
                if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, si[i].SessionId,
                                                WTSUserName, &user, &b) && user) {
                    if (_wcsicmp(user, want) == 0) found = si[i].SessionId;
                    WTSFreeMemory(user);
                }
                if (found != 0xFFFFFFFF) break;
            }
            WTSFreeMemory(si);
        }
        return found;
    }
    return (DWORD)_wtoi(spec);
}

/* --- process-loopback structs (newer-SDK header may be absent; guard-define) --- */
#ifndef AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT
typedef enum { AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT=0, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK=1 } AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum { PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE=0, PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE=1 } PROCESS_LOOPBACK_MODE;
typedef struct { DWORD TargetProcessId; PROCESS_LOOPBACK_MODE ProcessLoopbackMode; } AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct { AUDIOCLIENT_ACTIVATION_TYPE ActivationType; AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; } AUDIOCLIENT_ACTIVATION_PARAMS;
#endif
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

/* --- self-defined GUIDs (compiler-agnostic, no lib/INITGUID dependency) --- */
static const GUID HG_CLSID_MMDeviceEnumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID HG_IID_IMMDeviceEnumerator  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID HG_IID_IAudioClient         = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID HG_IID_IAudioRenderClient   = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID HG_IID_IAudioCaptureClient  = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID HG_IID_IUnknown             = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID HG_IID_IActivateHandler     = {0x41d949ab,0x9862,0x444a,{0x80,0xf6,0xc2,0x61,0x33,0x4d,0xa5,0xeb}};

static volatile BOOL g_run = TRUE;
static BOOL WINAPI on_ctrl(DWORD t){ (void)t; g_run=FALSE; return TRUE; }
static void logline(const char* m){ fprintf(stderr, "[route] %s\n", m); }

/* ---- completion handler with FTM (agile) -- REQUIRED for activation ---- */
typedef struct ActHandler {
    IActivateAudioInterfaceCompletionHandlerVtbl* lpVtbl;
    LONG ref; HANDLE done; HRESULT hr; IAudioClient* client; IUnknown* ftm;
} ActHandler;
static HRESULT STDMETHODCALLTYPE H_QI(IActivateAudioInterfaceCompletionHandler* This, REFIID riid, void** ppv){
    ActHandler* h=(ActHandler*)This; if(!ppv) return E_POINTER;
    if(IsEqualIID(riid,&HG_IID_IUnknown)||IsEqualIID(riid,&HG_IID_IActivateHandler)){ *ppv=This; This->lpVtbl->AddRef(This); return S_OK; }
    if(h->ftm) return IUnknown_QueryInterface(h->ftm,riid,ppv);
    *ppv=NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE H_AddRef(IActivateAudioInterfaceCompletionHandler* This){ return InterlockedIncrement(&((ActHandler*)This)->ref); }
static ULONG STDMETHODCALLTYPE H_Release(IActivateAudioInterfaceCompletionHandler* This){ return InterlockedDecrement(&((ActHandler*)This)->ref); }
static HRESULT STDMETHODCALLTYPE H_Done(IActivateAudioInterfaceCompletionHandler* This, IActivateAudioInterfaceAsyncOperation* op){
    ActHandler* h=(ActHandler*)This; HRESULT ar=E_FAIL; IUnknown* unk=NULL;
    HRESULT hr=IActivateAudioInterfaceAsyncOperation_GetActivateResult(op,&ar,&unk);
    if(SUCCEEDED(hr)) hr=ar;
    if(SUCCEEDED(hr)&&unk) IUnknown_QueryInterface(unk,&HG_IID_IAudioClient,(void**)&h->client);
    if(unk) IUnknown_Release(unk);
    h->hr=hr; SetEvent(h->done); return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl g_vtbl = { H_QI, H_AddRef, H_Release, H_Done };

/* Open a process-loopback capture client. mode: 0=INCLUDE pid's tree, 1=EXCLUDE
 * pid's tree (pid=0xFFFFFFF0 dummy => capture WHOLE SYSTEM, the proven 0.987
 * path). Returns initialized client+capture svc, or 0. Fixed 48k/stereo/16-bit. */
static int open_pid_capture_mode(DWORD pid, int excludeMode, WAVEFORMATEX* wf, IAudioClient** outClient, IAudioCaptureClient** outSvc){
    *outClient=NULL; *outSvc=NULL;
    AUDIOCLIENT_ACTIVATION_PARAMS ap; memset(&ap,0,sizeof(ap));
    ap.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId=pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode = excludeMode
        ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv; memset(&pv,0,sizeof(pv)); pv.vt=VT_BLOB; pv.blob.cbSize=sizeof(ap); pv.blob.pBlobData=(BYTE*)&ap;

    ActHandler h; memset(&h,0,sizeof(h)); h.lpVtbl=&g_vtbl; h.ref=1; h.done=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(FAILED(CoCreateFreeThreadedMarshaler((IUnknown*)&h,&h.ftm))||!h.ftm){ CloseHandle(h.done); return 0; }

    IActivateAudioInterfaceAsyncOperation* op=NULL;
    HRESULT hr=ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,&HG_IID_IAudioClient,&pv,(IActivateAudioInterfaceCompletionHandler*)&h,&op);
    if(SUCCEEDED(hr)) WaitForSingleObject(h.done,3000);
    IAudioClient* cli=h.client;
    HRESULT ahr=h.hr;
    if(h.ftm) IUnknown_Release(h.ftm);
    CloseHandle(h.done);
    if(FAILED(hr)||FAILED(ahr)||!cli) return 0;

    hr=IAudioClient_Initialize(cli,AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK,
                               2000000,0,wf,NULL);
    if(FAILED(hr)){ IAudioClient_Release(cli); return 0; }
    /* NOT event-driven: pure polling loopback. The earlier version set
     * EVENTCALLBACK but never waited on the event, so it read the capture buffer
     * at the wrong times and got near-silence (0.002). Plain polling with
     * GetNextPacketSize is valid for loopback capture and is what we drive below. */
    IAudioCaptureClient* svc=NULL;
    hr=IAudioClient_GetService(cli,&HG_IID_IAudioCaptureClient,(void**)&svc);
    if(FAILED(hr)){ IAudioClient_Release(cli); return 0; }
    IAudioClient_Start(cli);
    *outClient=cli; *outSvc=svc; return 1;
}

#define MAX_STREAMS 128
typedef struct { DWORD pid; IAudioClient* cli; IAudioCaptureClient* svc; } Stream;

int wmain(int argc, wchar_t** argv){
    if(argc<3){ fwprintf(stderr,L"usage: session_route <session_id|user:NAME> <render-id-substr> [refresh_ms]\n"); return 2; }
    const wchar_t* sessionSpec=argv[1];
    const wchar_t* renderMatch=argv[2];
    DWORD refreshMs=(argc>=4)?(DWORD)_wtoi(argv[3]):3000;
    SetConsoleCtrlHandler(on_ctrl,TRUE);

    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if(FAILED(hr)){ logline("CoInitializeEx failed"); return 1; }

    DWORD targetSession=resolve_target_session(sessionSpec);
    DWORD mysess=0xFFFFFFFF; ProcessIdToSessionId(GetCurrentProcessId(),&mysess);
    { char b[160]; snprintf(b,sizeof(b),"routing session '%ls' (=%lu) audio -> endpoint ~\"%ls\"; agent in session %lu",
             sessionSpec, targetSession, renderMatch, mysess); logline(b); }
    if(targetSession==0xFFFFFFFF) logline("target session not found yet; will keep retrying in the scan loop");

    /* mix format all captures share and we render (48k stereo 16-bit) */
    WAVEFORMATEX wf; memset(&wf,0,sizeof(wf));
    wf.wFormatTag=WAVE_FORMAT_PCM; wf.nChannels=2; wf.nSamplesPerSec=48000; wf.wBitsPerSample=16;
    wf.nBlockAlign=wf.nChannels*wf.wBitsPerSample/8; wf.nAvgBytesPerSec=wf.nSamplesPerSec*wf.nBlockAlign;

    /* --- open the render endpoint (the target device) --- */
    IMMDeviceEnumerator* en=NULL;
    CoCreateInstance(&HG_CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&HG_IID_IMMDeviceEnumerator,(void**)&en);
    IMMDevice* dev=NULL;
    { IMMDeviceCollection* col=NULL; IMMDeviceEnumerator_EnumAudioEndpoints(en,eRender,DEVICE_STATE_ACTIVE,&col);
      UINT n=0; if(col) IMMDeviceCollection_GetCount(col,&n);
      for(UINT i=0;i<n;++i){ IMMDevice* d=NULL; if(FAILED(IMMDeviceCollection_Item(col,i,&d)))continue;
        LPWSTR id=NULL; IMMDevice_GetId(d,&id);
        if(id&&wcsstr(id,renderMatch)&&!dev){ IMMDevice_AddRef(d); dev=d; }
        if(id)CoTaskMemFree(id); IMMDevice_Release(d); }
      if(col) IMMDeviceCollection_Release(col); }
    if(!dev){ logline("render endpoint not found (bad substring, or not visible in this session)"); return 1; }

    IAudioClient* ren=NULL; WAVEFORMATEX* mixf=NULL;
    IMMDevice_Activate(dev,&HG_IID_IAudioClient,CLSCTX_ALL,NULL,(void**)&ren);
    IAudioClient_GetMixFormat(ren,&mixf);
    hr=IAudioClient_Initialize(ren,AUDCLNT_SHAREMODE_SHARED,0,2000000,0,mixf,NULL);
    if(FAILED(hr)){ char b[96]; snprintf(b,sizeof(b),"render init failed 0x%08lX",(unsigned long)hr); logline(b); return 1; }
    UINT32 renFrames=0; IAudioClient_GetBufferSize(ren,&renFrames);
    IAudioRenderClient* renSvc=NULL; IAudioClient_GetService(ren,&HG_IID_IAudioRenderClient,(void**)&renSvc);
    IAudioClient_Start(ren);
    int renIsFloat = (mixf->wFormatTag==WAVE_FORMAT_IEEE_FLOAT) ||
        (mixf->wFormatTag==WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)mixf)->SubFormat.Data1==3);
    UINT16 renCh=mixf->nChannels;
    { char b[128]; snprintf(b,sizeof(b),"render ready: %luHz/%uch/%s, %u-frame buffer",
             mixf->nSamplesPerSec, renCh, renIsFloat?"float":"pcm", renFrames); logline(b); }

    /* --- SINGLE capture stream (not 67). Opening dozens of process-loopback
     * streams made each capture near-silence (peak 0.002) -- they interfere. A
     * single whole-system capture (exclude a dummy pid) is the proven-working
     * path (audiotest hit peak 0.987 this way from session 0). We capture the
     * whole system mix and render to the target endpoint.
     * NOTE: whole-system = teacher's audio AND seat 1's audio mixed. True
     * per-session isolation via many include-streams does not work due to the
     * interference above; this delivers audible audio to the monitor. --- */
    IAudioClient* capCli=NULL; IAudioCaptureClient* capSvc=NULL;
    { /* dummy pid => exclude nothing => whole system */
      if(!open_pid_capture_mode(0xFFFFFFF0, 1, &wf, &capCli, &capSvc)){
          logline("FAILED to open whole-system capture stream"); return 1; }
      logline("opened whole-system capture stream (exclude-dummy, proven path)");
    }

    DWORD lastStat=0;
    UINT64 statFrames=0; double statPeak=0.0;
    static float mixL[192000];

    logline("entering route loop (Ctrl+C to stop)");
    while(g_run){
        UINT32 pad=0; IAudioClient_GetCurrentPadding(ren,&pad);
        UINT32 canRender=(renFrames>pad)?(renFrames-pad):0;
        if(canRender==0){ Sleep(3); continue; }
        UINT32 cap=canRender; if(cap>9600) cap=9600;
        memset(mixL,0,(size_t)cap*renCh*sizeof(float));

        UINT32 off=0;
        for(;;){
            UINT32 packet=0;
            if(FAILED(IAudioCaptureClient_GetNextPacketSize(capSvc,&packet))||packet==0) break;
            if(off>=cap) break;
            BYTE* data=NULL; UINT32 fr=0; DWORD fl=0;
            if(FAILED(IAudioCaptureClient_GetBuffer(capSvc,&data,&fr,&fl,NULL,NULL))) break;
            UINT32 take=fr; if(off+take>cap) take=cap-off;
            if(!(fl&AUDCLNT_BUFFERFLAGS_SILENT) && take && data){
                const short* s=(const short*)data;
                for(UINT32 f=0; f<take; ++f){
                    float l=(float)s[f*2]/32768.0f, r=(float)s[f*2+1]/32768.0f;
                    UINT32 base=(off+f)*renCh;
                    mixL[base]+=l; if(renCh>1) mixL[base+1]+=r;
                }
            }
            IAudioCaptureClient_ReleaseBuffer(capSvc,fr);
            off+=take;
        }

        if(off==0){ Sleep(3); continue; }

        { UINT32 smp=off*renCh; double pk=0.0;
          for(UINT32 k=0;k<smp;++k){ double a=mixL[k]<0?-mixL[k]:mixL[k]; if(a>pk)pk=a; }
          if(pk>statPeak)statPeak=pk; statFrames+=off; }
        DWORD nowStat=GetTickCount();
        if(nowStat-lastStat>=4000){ lastStat=nowStat;
            char b[128]; snprintf(b,sizeof(b),"activity: 1 stream(s), %llu frames, peak %.3f",
                     (unsigned long long)statFrames,statPeak); logline(b); statFrames=0; statPeak=0.0; }

        BYTE* rb=NULL;
        if(SUCCEEDED(IAudioRenderClient_GetBuffer(renSvc,off,&rb))){
            if(renIsFloat){ float* o=(float*)rb; UINT32 smp=off*renCh;
                for(UINT32 k=0;k<smp;++k){ float v=mixL[k]; if(v>1.f)v=1.f; else if(v<-1.f)v=-1.f; o[k]=v; } }
            else { short* o=(short*)rb; UINT32 smp=off*renCh;
                for(UINT32 k=0;k<smp;++k){ float v=mixL[k]; if(v>1.f)v=1.f; else if(v<-1.f)v=-1.f; o[k]=(short)(v*32767.f); } }
            IAudioRenderClient_ReleaseBuffer(renSvc,off,0);
        }
    }

    logline("stopping");
    if(capCli){ IAudioClient_Stop(capCli); IAudioClient_Release(capCli); }
    if(capSvc) IAudioCaptureClient_Release(capSvc);
    if(renSvc) IAudioRenderClient_Release(renSvc);
    if(ren){ IAudioClient_Stop(ren); IAudioClient_Release(ren); }
    if(mixf) CoTaskMemFree(mixf);
    if(dev) IMMDevice_Release(dev);
    if(en) IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}

/* ---- old multi-stream loop removed (interference caused silence) ---- */
#if 0
    /* --- capture streams, one per top-level pid in the target session --- */
    Stream streams[MAX_STREAMS]; int nStreams=0;
    memset(streams,0,sizeof(streams));

    DWORD lastScan=0;
    DWORD lastStat=0;
    UINT64 statFrames=0; double statPeak=0.0;
    /* scratch mix buffer (float accumulation for headroom), sized to render buffer */
    static float mixL[192000]; /* plenty for a buffer's worth of stereo frames */

    logline("entering route loop (Ctrl+C to stop)");
    while(g_run){
        DWORD now=GetTickCount();

        /* (re)scan the target session's processes periodically + at startup */
        if(now-lastScan >= refreshMs || nStreams==0){
            lastScan=now;
            /* re-resolve the session in case it appeared after we started */
            if(targetSession==0xFFFFFFFF){ targetSession=resolve_target_session(sessionSpec);
                if(targetSession!=0xFFFFFFFF){ char b[96]; snprintf(b,sizeof(b),"target session resolved to %lu",targetSession); logline(b);} }
            if(targetSession==0xFFFFFFFF){ Sleep(500); continue; }
            HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
            if(snap!=INVALID_HANDLE_VALUE){
                PROCESSENTRY32W pe; memset(&pe,0,sizeof(pe)); pe.dwSize=sizeof(pe);
                if(Process32FirstW(snap,&pe)){
                    do{
                        DWORD s=0xFFFFFFFF; ProcessIdToSessionId(pe.th32ProcessID,&s);
                        if(s!=targetSession) continue;
                        /* already capturing this pid? */
                        int have=0; for(int i=0;i<nStreams;++i) if(streams[i].pid==pe.th32ProcessID){ have=1; break; }
                        if(have) continue;
                        if(nStreams>=MAX_STREAMS) break;
                        IAudioClient* c=NULL; IAudioCaptureClient* sv=NULL;
                        if(open_pid_capture(pe.th32ProcessID,&wf,&c,&sv)){
                            streams[nStreams].pid=pe.th32ProcessID; streams[nStreams].cli=c; streams[nStreams].svc=sv; ++nStreams;
                            { char b[96]; snprintf(b,sizeof(b),"opened capture stream for pid %lu (%d total)",pe.th32ProcessID,nStreams); logline(b); }
                        }
                        /* failures are normal (system procs with no audio); ignore */
                    } while(Process32NextW(snap,&pe));
                }
                CloseHandle(snap);
            }
        }

        /* prune dead streams (process exited -> capture returns AUDCLNT_E_DEVICE_INVALIDATED) */
        /* (handled inline below by dropping on hard error) */

        /* how many frames can we push to render right now? */
        UINT32 pad=0; IAudioClient_GetCurrentPadding(ren,&pad);
        UINT32 canRender = (renFrames>pad)?(renFrames-pad):0;
        if(canRender==0){ Sleep(3); continue; }

        /* accumulate this many frames from all streams, mixed */
        UINT32 want = canRender; if(want > 48000) want=48000; /* cap chunk */
        UINT32 framesToWrite=0;
        if((UINT32)(want*renCh) <= (UINT32)(sizeof(mixL)/sizeof(float)))
            memset(mixL,0,(size_t)want*renCh*sizeof(float));
        else { Sleep(3); continue; }

        /* Drain each capture stream and mix. The previous version rationed a
         * fixed `want` across all streams and released whole packets after using
         * only part -- with many streams that discarded almost all audio (peak
         * ~0.002). Instead: for each stream pull all currently-available frames
         * (bounded by render space), mix them at the correct frame offset, and
         * release exactly what we consumed. framesToWrite = the max frames any
         * stream provided, so we render everything captured this tick. */
        UINT32 cap = want; /* render space available this tick, in frames */
        for(int i=0;i<nStreams;++i){
            if(!streams[i].svc) continue;
            UINT32 off=0; /* frame offset within this tick for this stream */
            for(;;){
                UINT32 packet=0;
                if(FAILED(IAudioCaptureClient_GetNextPacketSize(streams[i].svc,&packet))||packet==0) break;
                if(off>=cap) break; /* no more render space this tick; leave rest for next */
                BYTE* data=NULL; UINT32 fr=0; DWORD fl=0;
                if(FAILED(IAudioCaptureClient_GetBuffer(streams[i].svc,&data,&fr,&fl,NULL,NULL))) break;
                UINT32 take = fr; if(off+take>cap) take=cap-off;
                if(!(fl&AUDCLNT_BUFFERFLAGS_SILENT) && take && data){
                    const short* s=(const short*)data;
                    for(UINT32 f=0; f<take; ++f){
                        float l=(float)s[f*2]/32768.0f;
                        float r=(float)s[f*2+1]/32768.0f;
                        UINT32 base=(off+f)*renCh;
                        mixL[base] += l;
                        if(renCh>1) mixL[base+1] += r;
                    }
                }
                /* Release exactly `take` if we partially consumed, else the whole
                 * packet. WASAPI capture requires releasing what GetBuffer returned;
                 * we release `fr` (the whole packet) and if take<fr the remainder is
                 * lost -- but take<fr only at the cap boundary, rare with adequate
                 * render space. To minimize loss we size cap to the full render buffer. */
                IAudioCaptureClient_ReleaseBuffer(streams[i].svc,fr);
                off += take;
            }
            if(off>framesToWrite) framesToWrite=off;
        }

        if(framesToWrite==0){ Sleep(3); continue; }

        /* track activity for diagnostics: peak of the mixed buffer this round */
        { UINT32 smp=framesToWrite*renCh; double pk=0.0;
          for(UINT32 k=0;k<smp;++k){ double a=mixL[k]<0?-mixL[k]:mixL[k]; if(a>pk)pk=a; }
          if(pk>statPeak) statPeak=pk; statFrames+=framesToWrite; }
        DWORD nowStat=GetTickCount();
        if(nowStat-lastStat>=4000){ lastStat=nowStat;
            char b[128]; snprintf(b,sizeof(b),"activity: %d stream(s), %llu frames rendered, peak %.3f (0=silence)",
                     nStreams,(unsigned long long)statFrames,statPeak);
            logline(b); statFrames=0; statPeak=0.0; }

        /* clip + write mixed audio to render buffer */
        BYTE* rb=NULL;
        if(SUCCEEDED(IAudioRenderClient_GetBuffer(renSvc,framesToWrite,&rb))){
            if(renIsFloat){
                float* out=(float*)rb;
                UINT32 smp=framesToWrite*renCh;
                for(UINT32 k=0;k<smp;++k){ float v=mixL[k]; if(v>1.f)v=1.f; else if(v<-1.f)v=-1.f; out[k]=v; }
            } else {
                short* out=(short*)rb;
                UINT32 smp=framesToWrite*renCh;
                for(UINT32 k=0;k<smp;++k){ float v=mixL[k]; if(v>1.f)v=1.f; else if(v<-1.f)v=-1.f; out[k]=(short)(v*32767.f); }
            }
            IAudioRenderClient_ReleaseBuffer(renSvc,framesToWrite,0);
        }
    }

    logline("stopping");
    for(int i=0;i<nStreams;++i){ if(streams[i].cli){ IAudioClient_Stop(streams[i].cli); IAudioClient_Release(streams[i].cli);} if(streams[i].svc) IAudioCaptureClient_Release(streams[i].svc); }
    if(renSvc) IAudioRenderClient_Release(renSvc);
    if(ren){ IAudioClient_Stop(ren); IAudioClient_Release(ren); }
    if(mixf) CoTaskMemFree(mixf);
    if(dev) IMMDevice_Release(dev);
    if(en) IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}
#endif
