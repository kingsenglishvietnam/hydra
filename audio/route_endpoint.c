/* route_endpoint.c  --  endpoint-loopback one device, render to another.
 *
 * THE WORKING PATH (after process-loopback failed from session 0)
 *   Process-loopback from a session-0 service activates but delivers SILENCE
 *   (confirmed: audiotest peak 0.001 from the real service). Plain ENDPOINT
 *   loopback, however, is documented to work cross-session from a session-0
 *   service ("run a loopback client in a session 0 service and capture audio
 *   from all user sessions"). So instead of capturing teacher's PROCESSES, we:
 *
 *     teacher's session output -> VB-CABLE (virtual device)
 *     THIS agent (session 0)   -> endpoint-loopback the CABLE -> render to monitor
 *     seat 1                   -> laptop (untouched)
 *
 *   The virtual cable is the bridge across the session boundary that process-
 *   loopback couldn't be. Endpoint loopback reads the cable's real stream, which
 *   the audio engine DOES deliver cross-session from session 0.
 *
 *   Isolation is clean: teacher -> cable -> monitor; you -> laptop. Two separate
 *   device paths.
 *
 * BUILD:  cl /O2 route_endpoint.c /link ole32.lib mmdevapi.lib
 * USAGE:  route_endpoint.exe <source-id-substr> <render-id-substr> [--list]
 *           source = the CABLE's endpoint id substring (what to loopback-capture)
 *           render = the monitor's endpoint id substring (where to send it)
 *           --list = enumerate ALL render endpoints (find the cable + monitor ids)
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

static const GUID HG_CLSID_MMDeviceEnumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID HG_IID_IMMDeviceEnumerator  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID HG_IID_IAudioClient         = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID HG_IID_IAudioRenderClient   = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID HG_IID_IAudioCaptureClient  = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const PROPERTYKEY HG_PKEY_FriendlyName = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};

static volatile BOOL g_run=TRUE;
static BOOL WINAPI on_ctrl(DWORD t){ (void)t; g_run=FALSE; return TRUE; }
static void L(const char* m){ fprintf(stderr,"[eroute] %s\n",m); }

static IMMDevice* find_render(IMMDeviceEnumerator* en, const wchar_t* match, int list){
    IMMDeviceCollection* col=NULL;
    IMMDeviceEnumerator_EnumAudioEndpoints(en,eRender,DEVICE_STATE_ACTIVE,&col);
    UINT n=0; if(col) IMMDeviceCollection_GetCount(col,&n);
    IMMDevice* found=NULL;
    for(UINT i=0;i<n;++i){ IMMDevice* d=NULL; if(FAILED(IMMDeviceCollection_Item(col,i,&d)))continue;
        LPWSTR id=NULL; IMMDevice_GetId(d,&id);
        wchar_t fn[256]=L"?"; IPropertyStore* ps=NULL;
        if(SUCCEEDED(IMMDevice_OpenPropertyStore(d,STGM_READ,&ps))){ PROPVARIANT v; PropVariantInit(&v);
            if(SUCCEEDED(IPropertyStore_GetValue(ps,&HG_PKEY_FriendlyName,&v))&&v.vt==VT_LPWSTR) wcsncpy_s(fn,256,v.pwszVal,_TRUNCATE);
            PropVariantClear(&v); IPropertyStore_Release(ps); }
        if(list) fwprintf(stderr,L"  render: %-45ls id=%ls\n",fn,id?id:L"?");
        if(match&&id&&wcsstr(id,match)&&!found){ IMMDevice_AddRef(d); found=d;
            fwprintf(stderr,L"[eroute] matched: %ls\n",fn); }
        if(id)CoTaskMemFree(id); IMMDevice_Release(d);
    }
    if(col) IMMDeviceCollection_Release(col);
    return found;
}

int wmain(int argc, wchar_t** argv){
    if(argc<2){ fwprintf(stderr,L"usage: route_endpoint <source-substr> <render-substr>\n       route_endpoint --list\n"); return 2; }
    SetConsoleCtrlHandler(on_ctrl,TRUE);
    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if(FAILED(hr)){ L("CoInitializeEx failed"); return 1; }

    IMMDeviceEnumerator* en=NULL;
    CoCreateInstance(&HG_CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&HG_IID_IMMDeviceEnumerator,(void**)&en);

    if(wcscmp(argv[1],L"--list")==0){ L("active render endpoints:"); find_render(en,NULL,1); CoUninitialize(); return 0; }
    if(argc<3){ fwprintf(stderr,L"need <source-substr> <render-substr>\n"); return 2; }

    const wchar_t* srcMatch=argv[1];
    const wchar_t* dstMatch=argv[2];
    { char b[160]; snprintf(b,sizeof(b),"endpoint-loopback source ~\"%ls\" -> render ~\"%ls\"",srcMatch,dstMatch); L(b); }

    /* SOURCE: the cable's render endpoint, opened in LOOPBACK capture mode */
    IMMDevice* srcDev=find_render(en,srcMatch,0);
    if(!srcDev){ L("source endpoint not found"); return 1; }
    IMMDevice* dstDev=find_render(en,dstMatch,0);
    if(!dstDev){ L("render endpoint not found"); return 1; }

    IAudioClient* cap=NULL; WAVEFORMATEX* capfmt=NULL;
    IMMDevice_Activate(srcDev,&HG_IID_IAudioClient,CLSCTX_ALL,NULL,(void**)&cap);
    IAudioClient_GetMixFormat(cap,&capfmt);
    hr=IAudioClient_Initialize(cap,AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,2000000,0,capfmt,NULL);
    if(FAILED(hr)){ char b[96]; snprintf(b,sizeof(b),"capture init failed 0x%08lX",(unsigned long)hr); L(b); return 1; }
    IAudioCaptureClient* capSvc=NULL; IAudioClient_GetService(cap,&HG_IID_IAudioCaptureClient,(void**)&capSvc);

    /* RENDER: the monitor */
    IAudioClient* ren=NULL; WAVEFORMATEX* renfmt=NULL;
    IMMDevice_Activate(dstDev,&HG_IID_IAudioClient,CLSCTX_ALL,NULL,(void**)&ren);
    IAudioClient_GetMixFormat(ren,&renfmt);
    hr=IAudioClient_Initialize(ren,AUDCLNT_SHAREMODE_SHARED,0,2000000,0,renfmt,NULL);
    if(FAILED(hr)){ char b[96]; snprintf(b,sizeof(b),"render init failed 0x%08lX",(unsigned long)hr); L(b); return 1; }
    UINT32 renFrames=0; IAudioClient_GetBufferSize(ren,&renFrames);
    IAudioRenderClient* renSvc=NULL; IAudioClient_GetService(ren,&HG_IID_IAudioRenderClient,(void**)&renSvc);

    /* formats: capture (cable mix) and render (monitor mix). If rate/channels
     * differ, bail clearly. Both are usually 48k stereo float. */
    if(capfmt->nSamplesPerSec!=renfmt->nSamplesPerSec || capfmt->nChannels!=renfmt->nChannels){
        char b[160]; snprintf(b,sizeof(b),"format mismatch: src %luHz/%uch vs dst %luHz/%uch -- match them in Sound settings",
            capfmt->nSamplesPerSec,capfmt->nChannels,renfmt->nSamplesPerSec,renfmt->nChannels); L(b); return 1; }
    int srcFloat=(capfmt->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)||(capfmt->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&((WAVEFORMATEXTENSIBLE*)capfmt)->SubFormat.Data1==3);
    int dstFloat=(renfmt->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)||(renfmt->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&((WAVEFORMATEXTENSIBLE*)renfmt)->SubFormat.Data1==3);
    UINT16 bytesSrc=capfmt->wBitsPerSample/8, bytesDst=renfmt->wBitsPerSample/8, ch=renfmt->nChannels;

    IAudioClient_Start(cap); IAudioClient_Start(ren);
    { char b[128]; snprintf(b,sizeof(b),"running: %luHz/%uch, src %s / dst %s, %u-frame buf",
        renfmt->nSamplesPerSec,ch,srcFloat?"float":"pcm",dstFloat?"float":"pcm",renFrames); L(b); }

    DWORD lastStat=0; UINT64 sf=0; double sp=0.0;
    while(g_run){
        UINT32 packet=0;
        if(FAILED(IAudioCaptureClient_GetNextPacketSize(capSvc,&packet))){ Sleep(3); continue; }
        if(packet==0){ Sleep(3); continue; }
        BYTE* data=NULL; UINT32 fr=0; DWORD fl=0;
        if(FAILED(IAudioCaptureClient_GetBuffer(capSvc,&data,&fr,&fl,NULL,NULL))){ Sleep(3); continue; }
        UINT32 pad=0; IAudioClient_GetCurrentPadding(ren,&pad);
        UINT32 avail=renFrames-pad; UINT32 todo=fr<avail?fr:avail;
        if(todo){
            BYTE* rb=NULL;
            if(SUCCEEDED(IAudioRenderClient_GetBuffer(renSvc,todo,&rb))){
                if(fl&AUDCLNT_BUFFERFLAGS_SILENT){ memset(rb,0,(size_t)todo*bytesDst*ch); }
                else if(srcFloat&&dstFloat){ memcpy(rb,data,(size_t)todo*bytesDst*ch);
                    /* peak */ const float* s=(const float*)data; UINT32 smp=todo*ch; for(UINT32 i=0;i<smp;++i){ double a=s[i]<0?-s[i]:s[i]; if(a>sp)sp=a; } }
                else if(!srcFloat&&!dstFloat){ memcpy(rb,data,(size_t)todo*bytesDst*ch);
                    const short* s=(const short*)data; UINT32 smp=todo*ch; for(UINT32 i=0;i<smp;++i){ double a=(s[i]<0?-s[i]:s[i])/32768.0; if(a>sp)sp=a; } }
                else if(srcFloat&&!dstFloat){ const float* s=(const float*)data; short* o=(short*)rb; UINT32 smp=todo*ch;
                    for(UINT32 i=0;i<smp;++i){ float f=s[i]; if(f>1)f=1; else if(f<-1)f=-1; o[i]=(short)(f*32767.f); double a=f<0?-f:f; if(a>sp)sp=a; } }
                else { const short* s=(const short*)data; float* o=(float*)rb; UINT32 smp=todo*ch;
                    for(UINT32 i=0;i<smp;++i){ o[i]=(float)s[i]/32768.f; double a=(s[i]<0?-s[i]:s[i])/32768.0; if(a>sp)sp=a; } }
                IAudioRenderClient_ReleaseBuffer(renSvc,todo,0);
                sf+=todo;
            }
        }
        IAudioCaptureClient_ReleaseBuffer(capSvc,fr);
        DWORD now=GetTickCount();
        if(now-lastStat>=4000){ lastStat=now; char b[128]; snprintf(b,sizeof(b),"activity: %llu frames, peak %.3f",(unsigned long long)sf,sp); L(b); sf=0; sp=0.0; }
    }

    L("stopping");
    IAudioClient_Stop(cap); IAudioClient_Stop(ren);
    if(capfmt)CoTaskMemFree(capfmt); if(renfmt)CoTaskMemFree(renfmt);
    if(capSvc)IAudioCaptureClient_Release(capSvc); if(renSvc)IAudioRenderClient_Release(renSvc);
    if(cap)IAudioClient_Release(cap); if(ren)IAudioClient_Release(ren);
    if(srcDev)IMMDevice_Release(srcDev); if(dstDev)IMMDevice_Release(dstDev);
    if(en)IMMDeviceEnumerator_Release(en); CoUninitialize();
    return 0;
}
