/* mutetest.c  --  can we silence teacher's DIRECT output to an endpoint while
 * session_route still captures it upstream? The audio "/dev/null" experiment.
 *
 * IDEA
 *   process-loopback captures an app's render stream BEFORE it's mixed to an
 *   endpoint. So if we MUTE teacher's app sessions ON the monitor endpoint, the
 *   physical monitor goes silent for teacher's direct path (= /dev/null), but
 *   session_route -- capturing upstream -- still gets the audio and renders it to
 *   the monitor itself. Net: teacher audio reaches the monitor ONLY via
 *   session_route, no feedback, no leak. If muting also kills the capture, this
 *   approach can't work and we need a sink device.
 *
 * WHAT THIS TOOL DOES
 *   On the endpoint whose id contains <substr>, enumerate its audio sessions,
 *   and for every session whose owning process is in <session_id> (teacher's TS
 *   session), mute it (or set volume 0). Reports what it muted. Runs from the
 *   session that can SEE the endpoint's sessions -- test both console and, via
 *   PsExec, session 0 / teacher's session, to find where it has effect.
 *
 *   Run it WHILE session_route is routing and audio is playing, then listen:
 *     - monitor still plays (via session_route) but teacher's DIRECT/dup path
 *       (the feedback/leak) goes quiet  -> SUCCESS, /dev/null works
 *     - session_route's audio ALSO goes quiet -> mute is downstream of nothing
 *       useful; capture is killed too -> need a sink device
 *
 * BUILD:  cl /O2 /EHsc mutetest.c /link ole32.lib mmdevapi.lib
 * USAGE:  mutetest.exe <session_id> <endpoint-id-substr> [unmute]
 *           unmute = pass the word "unmute" to REVERSE (restore) instead of mute
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const GUID HG_CLSID_MMDeviceEnumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID HG_IID_IMMDeviceEnumerator  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID HG_IID_IAudioSessionManager2= {0x77aa99a0,0x1bd6,0x484f,{0x8b,0xc7,0x2c,0x65,0x4c,0x9a,0x9b,0x6f}};
static const GUID HG_IID_IAudioSessionEnumerator={0xe2f5bb11,0x0570,0x40ca,{0xac,0xdd,0x3a,0xa0,0x12,0x77,0xde,0xe8}};
static const GUID HG_IID_IAudioSessionControl2= {0xbfb7ff88,0x7239,0x4fc9,{0x8f,0xa2,0x07,0xc9,0x50,0xbe,0x9c,0x6d}};
static const GUID HG_IID_ISimpleAudioVolume   = {0x87ce5498,0x68d6,0x44e5,{0x92,0x15,0x6d,0xa4,0x7e,0xf8,0x83,0xd8}};

int wmain(int argc, wchar_t** argv){
    if(argc<3){ fwprintf(stderr,L"usage: mutetest <session_id> <endpoint-id-substr> [unmute]\n"); return 2; }
    DWORD targetSession=(DWORD)_wtoi(argv[1]);
    const wchar_t* epMatch=argv[2];
    int doMute = !(argc>=4 && _wcsicmp(argv[3],L"unmute")==0);

    DWORD mysess=0xFFFFFFFF; ProcessIdToSessionId(GetCurrentProcessId(),&mysess);
    fwprintf(stderr,L"[mute] %s sessions of TS session %lu on endpoint ~\"%ls\"; this proc in session %lu\n",
             doMute?L"MUTING":L"UNMUTING", targetSession, epMatch, mysess);

    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if(FAILED(hr)){ fwprintf(stderr,L"CoInitializeEx failed\n"); return 1; }

    IMMDeviceEnumerator* en=NULL;
    hr=CoCreateInstance(&HG_CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&HG_IID_IMMDeviceEnumerator,(void**)&en);
    if(FAILED(hr)){ fwprintf(stderr,L"enumerator failed\n"); return 1; }

    /* find the endpoint */
    IMMDevice* dev=NULL;
    { IMMDeviceCollection* col=NULL; IMMDeviceEnumerator_EnumAudioEndpoints(en,eRender,DEVICE_STATE_ACTIVE,&col);
      UINT n=0; if(col) IMMDeviceCollection_GetCount(col,&n);
      for(UINT i=0;i<n;++i){ IMMDevice* d=NULL; if(FAILED(IMMDeviceCollection_Item(col,i,&d)))continue;
        LPWSTR id=NULL; IMMDevice_GetId(d,&id);
        if(id&&wcsstr(id,epMatch)&&!dev){ IMMDevice_AddRef(d); dev=d; }
        if(id)CoTaskMemFree(id); IMMDevice_Release(d); }
      if(col) IMMDeviceCollection_Release(col); }
    if(!dev){ fwprintf(stderr,L"[mute] endpoint not found (id substr \"%ls\") in THIS session\n",epMatch); return 1; }

    /* get the session manager for that endpoint */
    IAudioSessionManager2* mgr=NULL;
    hr=IMMDevice_Activate(dev,&HG_IID_IAudioSessionManager2,CLSCTX_ALL,NULL,(void**)&mgr);
    if(FAILED(hr)){ fwprintf(stderr,L"[mute] Activate(IAudioSessionManager2) failed 0x%08lX\n",(unsigned long)hr); return 1; }

    IAudioSessionEnumerator* se=NULL;
    hr=IAudioSessionManager2_GetSessionEnumerator(mgr,&se);
    if(FAILED(hr)){ fwprintf(stderr,L"[mute] GetSessionEnumerator failed 0x%08lX\n",(unsigned long)hr); return 1; }

    int count=0; IAudioSessionEnumerator_GetCount(se,&count);
    fwprintf(stderr,L"[mute] endpoint has %d audio session(s):\n",count);

    int affected=0;
    for(int i=0;i<count;++i){
        IAudioSessionControl* ctl=NULL;
        if(FAILED(IAudioSessionEnumerator_GetSession(se,i,&ctl))) continue;
        IAudioSessionControl2* ctl2=NULL;
        if(SUCCEEDED(IAudioSessionControl_QueryInterface(ctl,&HG_IID_IAudioSessionControl2,(void**)&ctl2))){
            DWORD pid=0; IAudioSessionControl2_GetProcessId(ctl2,&pid);
            DWORD ps=0xFFFFFFFF; if(pid) ProcessIdToSessionId(pid,&ps);
            fwprintf(stderr,L"   session %d: pid=%lu (TS session %lu)%ls\n",
                     i, pid, ps, (ps==targetSession)?L"  <-- TARGET":L"");
            if(ps==targetSession && pid!=0){
                ISimpleAudioVolume* vol=NULL;
                if(SUCCEEDED(IAudioSessionControl2_QueryInterface(ctl2,&HG_IID_ISimpleAudioVolume,(void**)&vol))){
                    hr=ISimpleAudioVolume_SetMute(vol,doMute?TRUE:FALSE,NULL);
                    if(SUCCEEDED(hr)){ ++affected;
                        fwprintf(stderr,L"        -> %s pid %lu on this endpoint\n",doMute?L"MUTED":L"UNMUTED",pid); }
                    else fwprintf(stderr,L"        -> SetMute failed 0x%08lX\n",(unsigned long)hr);
                    ISimpleAudioVolume_Release(vol);
                }
            }
            IAudioSessionControl2_Release(ctl2);
        }
        IAudioSessionControl_Release(ctl);
    }

    fwprintf(stderr,L"[mute] done: %d session(s) %s.\n",affected,doMute?L"muted":L"unmuted");
    if(doMute){
        fwprintf(stderr,L"[mute] NOW LISTEN: does teacher's DIRECT monitor output go quiet while\n"
                        L"       session_route's monitor audio keeps playing? If yes -- /dev/null works.\n"
                        L"       If session_route's audio ALSO died, capture is downstream of the mute\n"
                        L"       -> need a sink device. Run with 'unmute' to restore.\n");
    }

    IAudioSessionEnumerator_Release(se);
    IAudioSessionManager2_Release(mgr);
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}
