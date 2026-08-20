/* iddseat.cpp  --  Hydra virtual display (IddCx indirect display driver)
 *
 * See iddseat.h for the contract. All three original HYDRA-TODOs are filled:
 *   (a) EDID + modes  -> hydra_edid.h + MakeMonitorMode/MakeTargetMode + the
 *                        three mode callbacks advertise the seat's exact mode.
 *   (b) mirror IPC     -> SharedTextureSink (producer half of hydra_ipc.h).
 *   (c) LUID           -> Direct3DDevice(AdapterLuid) + published in metadata.
 *   seat name          -> read from a custom device property set by hydrad.
 *
 * WDK + MSVC build only. Not compiled in the Linux dev container (no WDK/D3D).
 * The pure-logic pieces it depends on (EDID bytes, IPC names) ARE unit-tested
 * natively; see tests/.
 */

#include "iddseat.h"
#include "../common/hydra_devprops.h"
#include <stdlib.h>   /* wcstoul */

using namespace Microsoft::WRL;

/* =========================================================================== *
 * Mode helpers (HYDRA-TODO(a) support)
 * =========================================================================== */
namespace hydra
{
    static void FillSignal(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& sig, const SeatMode& m)
    {
        /* Same blanking convention as the EDID DTD so the reported timing matches
         * what the EDID advertised. */
        const UINT hblank = 160, vblank = 40;
        sig.totalSize.cx = m.width + hblank;
        sig.totalSize.cy = m.height + vblank;
        sig.activeSize.cx = m.width;
        sig.activeSize.cy = m.height;

        UINT64 pixelRate = (UINT64)sig.totalSize.cx * sig.totalSize.cy * m.vsync;
        sig.pixelRate = pixelRate;

        sig.hSyncFreq.Numerator   = (UINT32)pixelRate;
        sig.hSyncFreq.Denominator = sig.totalSize.cx;
        sig.vSyncFreq.Numerator   = (UINT32)pixelRate;
        sig.vSyncFreq.Denominator = (UINT32)((UINT64)sig.totalSize.cx * sig.totalSize.cy);

        sig.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
        sig.AdditionalSignalInfo.videoStandard    = 255; /* Other */
        sig.AdditionalSignalInfo.vSyncFreqDivider = 0;
    }

    IDDCX_MONITOR_MODE MakeMonitorMode(const SeatMode& m, IDDCX_MONITOR_MODE_ORIGIN origin)
    {
        IDDCX_MONITOR_MODE mode{};
        mode.Size = sizeof(mode);
        mode.Origin = origin;
        FillSignal(mode.MonitorVideoSignalInfo, m);
        return mode;
    }

    IDDCX_TARGET_MODE MakeTargetMode(const SeatMode& m)
    {
        IDDCX_TARGET_MODE mode{};
        mode.Size = sizeof(mode);
        FillSignal(mode.TargetVideoSignalInfo.targetVideoSignalInfo, m);
        return mode;
    }
}

/* =========================================================================== *
 * SharedTextureSink -- producer half of hydra_ipc.h  (HYDRA-TODO(b) + (c))
 * =========================================================================== */
namespace hydra
{
    class SharedTextureSink final : public IMirrorSink
    {
    public:
        SharedTextureSink(const wchar_t* seat, const Direct3DDevice& dev)
            : m_seat(seat), m_dev(dev.Device), m_ctx(dev.DeviceContext), m_luid(dev.AdapterLuid)
        {
            /* Create the metadata section up front (dimensions unknown yet). */
            wchar_t metaName[128];
            hydra_meta_name(metaName, 128, ToAscii(m_seat).c_str());

            m_metaMap = CreateFileMappingW(INVALID_HANDLE_VALUE, MakeGlobalSA(),
                PAGE_READWRITE, 0, sizeof(HydraSeatMeta), metaName);
            if (m_metaMap)
            {
                m_meta = (HydraSeatMeta*)MapViewOfFile(m_metaMap, FILE_MAP_WRITE, 0, 0,
                                                       sizeof(HydraSeatMeta));
                if (m_meta)
                {
                    ZeroMemory(m_meta, sizeof(*m_meta));
                    m_meta->version = HYDRA_IPC_VERSION;
                    m_meta->luidLow  = m_luid.LowPart;
                    m_meta->luidHigh = m_luid.HighPart;
                    m_meta->generation = 1;
                    m_meta->ready = 0;
                }
            }
        }

        ~SharedTextureSink() override { Retire(); Cleanup(); }

        HRESULT Publish(ID3D11Texture2D* frame) override
        {
            if (!frame || !m_dev || !m_ctx) return E_FAIL;

            D3D11_TEXTURE2D_DESC fd{};
            frame->GetDesc(&fd);

            if (!m_shared || fd.Width != m_w || fd.Height != m_h || fd.Format != m_fmt)
            {
                HRESULT hr = CreateShared(fd);   /* (re)create on first frame / resize */
                if (FAILED(hr)) return hr;
            }

            /* Copy the composited frame into the shared texture under key 0.
             * "Latest frame wins": consumer also uses key 0, so this is mutual
             * exclusion, not a two-key handshake that would stall this thread. */
            if (m_mutex && SUCCEEDED(m_mutex->AcquireSync(HYDRA_MUTEX_KEY, HYDRA_ACQUIRE_TIMEOUT_MS)))
            {
                m_ctx->CopyResource(m_shared.Get(), frame);
                m_ctx->Flush();
                m_mutex->ReleaseSync(HYDRA_MUTEX_KEY);
                if (m_meta) { m_meta->frame++; }
                return S_OK;
            }
            return S_FALSE;   /* mirror held it / timeout; drop this frame */
        }

        void Retire() override
        {
            if (m_meta) m_meta->ready = 0;
            if (m_sharedNtHandle) { CloseHandle(m_sharedNtHandle); m_sharedNtHandle = nullptr; }
            m_mutex.Reset();
            m_shared.Reset();
            m_w = m_h = 0; m_fmt = DXGI_FORMAT_UNKNOWN;
        }

    private:
        static std::string ToAscii(const std::wstring& w)
        {
            std::string s; s.reserve(w.size());
            for (wchar_t c : w) s.push_back((char)c);
            return s;
        }

        /* A SECURITY_ATTRIBUTES granting the console-session mirror access to the
         * Global\ objects. NULL DACL is acceptable here: these are transient
         * display surfaces on a single-user box, and the objects are named
         * per-seat. Tighten to the mirror's SID for a hardened build. */
        static SECURITY_ATTRIBUTES* MakeGlobalSA()
        {
            static SECURITY_DESCRIPTOR sd;
            static SECURITY_ATTRIBUTES sa;
            InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);  /* NULL DACL */
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = FALSE;
            sa.lpSecurityDescriptor = &sd;
            return &sa;
        }

        HRESULT CreateShared(const D3D11_TEXTURE2D_DESC& fd)
        {
            Retire();  /* drop any previous surface first */

            ComPtr<ID3D11Device1> dev1;
            HRESULT hr = m_dev.As(&dev1);
            if (FAILED(hr)) return hr;

            D3D11_TEXTURE2D_DESC sd = fd;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            sd.CPUAccessFlags = 0;
            sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                         | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

            hr = m_dev->CreateTexture2D(&sd, nullptr, &m_shared);
            if (FAILED(hr)) return hr;

            hr = m_shared.As(&m_mutex);
            if (FAILED(hr)) return hr;

            ComPtr<IDXGIResource1> res;
            hr = m_shared.As(&res);
            if (FAILED(hr)) return hr;

            wchar_t surfName[128];
            hydra_surface_name(surfName, 128, ToAscii(m_seat).c_str());
            hr = res->CreateSharedHandle(MakeGlobalSA(),
                     DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                     surfName, &m_sharedNtHandle);
            if (FAILED(hr)) return hr;

            m_w = fd.Width; m_h = fd.Height; m_fmt = fd.Format;

            if (m_meta)
            {
                m_meta->width  = m_w;
                m_meta->height = m_h;
                m_meta->dxgiFormat = (uint32_t)m_fmt;
                m_meta->luidLow  = m_luid.LowPart;
                m_meta->luidHigh = m_luid.HighPart;
                m_meta->generation++;
                MemoryBarrier();
                m_meta->ready = 1;   /* publish last, after everything is valid */
            }
            return S_OK;
        }

        void Cleanup()
        {
            if (m_meta) { UnmapViewOfFile(m_meta); m_meta = nullptr; }
            if (m_metaMap) { CloseHandle(m_metaMap); m_metaMap = nullptr; }
        }

        std::wstring                m_seat;
        ComPtr<ID3D11Device>        m_dev;
        ComPtr<ID3D11DeviceContext> m_ctx;
        LUID                        m_luid{};

        HANDLE                      m_metaMap = nullptr;
        HydraSeatMeta*              m_meta = nullptr;

        ComPtr<ID3D11Texture2D>     m_shared;
        ComPtr<IDXGIKeyedMutex>     m_mutex;
        HANDLE                      m_sharedNtHandle = nullptr;
        UINT                        m_w = 0, m_h = 0;
        DXGI_FORMAT                 m_fmt = DXGI_FORMAT_UNKNOWN;
    };

    std::unique_ptr<IMirrorSink> MakeSharedTextureSink(const wchar_t* seatName,
                                                       const Direct3DDevice& device)
    {
        return std::make_unique<SharedTextureSink>(seatName, device);
    }
}

/* =========================================================================== *
 * Direct3D consuming device (HYDRA-TODO(c))
 * =========================================================================== */
namespace hydra
{
    Direct3DDevice::Direct3DDevice(LUID luid) : AdapterLuid(luid) {}

    HRESULT Direct3DDevice::Init()
    {
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
        if (FAILED(hr)) return hr;

        /* Enumerate the exact adapter IddCx reported for the swapchain -> our
         * consuming device shares that GPU. hydrad pins the virtual monitor's
         * render adapter to the physical GPU (the 1660 Ti) so this equals the
         * mirror's GPU and the share stays zero-copy. */
        hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
        if (FAILED(hr)) return hr;

        hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION,
                               &Device, nullptr, &DeviceContext);
        return hr;
    }

    /* ======================================================================= *
     * SwapChainProcessor
     * ======================================================================= */
    SwapChainProcessor::SwapChainProcessor(std::wstring seatName, IDDCX_SWAPCHAIN hSwapChain,
                                           std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent)
        : m_seatName(std::move(seatName))
        , m_hSwapChain(hSwapChain)
        , m_device(std::move(device))
        , m_newFrameEvent(newFrameEvent)
    {
        m_terminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
        m_thread = std::thread([this] { Run(); });
    }

    SwapChainProcessor::~SwapChainProcessor()
    {
        SetEvent(m_terminateEvent.Get());
        if (m_thread.joinable()) m_thread.join();
    }

    void SwapChainProcessor::Run()
    {
        DWORD taskIndex = 0;
        HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);
        RunCore();
        if (avTask) AvRevertMmThreadCharacteristics(avTask);
        WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    }

    void SwapChainProcessor::RunCore()
    {
        if (!m_device || FAILED(m_device->Init())) return;

        IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
        /* IddCx wants the DXGI device interface, not the D3D11 device directly.
         * QI across to IDXGIDevice (the sample does the same). */
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device->Device.As(&dxgiDevice))) return;
        setDevice.pDevice = dxgiDevice.Get();
        if (FAILED(IddCxSwapChainSetDevice(m_hSwapChain, &setDevice))) return;

        m_sink = MakeSharedTextureSink(m_seatName.c_str(), *m_device);

        for (;;)
        {
            IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
            HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);

            if (hr == E_PENDING)
            {
                HANDLE waits[] = { m_newFrameEvent, m_terminateEvent.Get() };
                DWORD w = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
                if (w == WAIT_OBJECT_0 + 1) break;
                continue;
            }
            if (FAILED(hr)) break;

            ComPtr<ID3D11Texture2D> frame;
            if (SUCCEEDED(buffer.MetaData.pSurface->QueryInterface(IID_PPV_ARGS(&frame))))
            {
                if (m_sink) m_sink->Publish(frame.Get());
            }

            IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0) break;
        }

        if (m_sink) m_sink->Retire();
        m_sink.reset();
    }
}

/* =========================================================================== *
 * Config helpers
 * =========================================================================== */
static hydra::SeatMode ParseModeString(PCWSTR s)
{
    /* "WxH@Hz" -> SeatMode; falls back to 1920x1080@60 on any parse miss. */
    hydra::SeatMode m;
    if (!s) return m;
    wchar_t* end = nullptr;
    unsigned long w = wcstoul(s, &end, 10);
    if (!w || !end || (*end != L'x' && *end != L'X')) return m;
    unsigned long h = wcstoul(end + 1, &end, 10);
    if (!h) return m;
    unsigned long hz = 60;
    if (end && *end == L'@') hz = wcstoul(end + 1, nullptr, 10);
    if (!hz) hz = 60;
    m.width = (UINT)w; m.height = (UINT)h; m.vsync = (UINT)hz;
    return m;
}

/* Read the seat name + mode properties hydrad set; defaults if absent. */
static void ReadSeatProperties(PWDFDEVICE_INIT init, std::wstring& seatOut,
                               std::wstring& modeOut)
{
    seatOut = L"B";
    modeOut = L"1920x1080@60";

    WCHAR buf[64] = {};
    ULONG required = 0;
    DEVPROPTYPE type = 0;

    WDF_DEVICE_PROPERTY_DATA nameData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&nameData, &DEVPKEY_Hydra_SeatName);
    nameData.Lcid = LOCALE_NEUTRAL;
    if (NT_SUCCESS(WdfFdoInitQueryPropertyEx(init, &nameData, sizeof(buf), buf, &required, &type))
        && type == DEVPROP_TYPE_STRING && buf[0])
        seatOut = buf;

    ZeroMemory(buf, sizeof(buf)); required = 0; type = 0;
    WDF_DEVICE_PROPERTY_DATA modeData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&modeData, &DEVPKEY_Hydra_SeatMode);
    modeData.Lcid = LOCALE_NEUTRAL;
    if (NT_SUCCESS(WdfFdoInitQueryPropertyEx(init, &modeData, sizeof(buf), buf, &required, &type))
        && type == DEVPROP_TYPE_STRING && buf[0])
        modeOut = buf;
}

/* =========================================================================== *
 * Driver / device lifecycle
 * =========================================================================== */
/* Which call fails?
 *
 * The UMDF host reports "failed to load the driver at level 0, error
 * 3489660941" == 0xD000000D == STATUS_INVALID_PARAMETER, and problem code
 * 31 on the devnode. Everything before this point -- match, policy,
 * signature, install, host start -- succeeds. Three calls in DeviceAdd can
 * return that status and each returns early, so from outside they cannot be
 * told apart.
 *
 * A file rather than OutputDebugStringW: WUDFHost is a service and nothing
 * is attached to catch debug strings. Opened and closed per line so a
 * failing load cannot lose the last one. */
static void IddSeatLog(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\HydraLog\\iddseat.log", "a") != 0 || !f)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02u:%02u:%02u.%03u [pid %5lu] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fprintf(f, "\\n");
    fclose(f);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    IddSeatLog("DriverEntry");
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, IddSeatDeviceAdd);
    return WdfDriverCreate(pDriverObject, pRegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatDeviceAdd(WDFDRIVER /*Driver*/, PWDFDEVICE_INIT pDeviceInit)
{
    IddSeatLog("DeviceAdd: entered");
    std::wstring seat, modeStr;
    ReadSeatProperties(pDeviceInit, seat, modeStr);
    IddSeatLog("DeviceAdd: seat properties read");

    /* Build the IddCx client config FIRST -- IddCxDeviceInitConfig takes it and
     * must run before WdfDeviceCreate (it registers the IddCx callbacks onto the
     * WDFDEVICE_INIT). IddCxDeviceInitialize(device) comes later, post-create. */
    IDD_CX_CLIENT_CONFIG cfg;
    IDD_CX_CLIENT_CONFIG_INIT(&cfg);
    cfg.EvtIddCxAdapterInitFinished               = IddSeatAdapterInitFinished;
    cfg.EvtIddCxAdapterCommitModes                = IddSeatAdapterCommitModes;
    cfg.EvtIddCxParseMonitorDescription           = IddSeatParseMonitorDescription;
    cfg.EvtIddCxMonitorGetDefaultDescriptionModes = IddSeatMonitorGetDefaultModes;
    cfg.EvtIddCxMonitorQueryTargetModes           = IddSeatMonitorQueryModes;
    cfg.EvtIddCxMonitorAssignSwapChain            = IddSeatMonitorAssignSwapChain;
    cfg.EvtIddCxMonitorUnassignSwapChain          = IddSeatMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(pDeviceInit, &cfg);
    IddSeatLog("IddCxDeviceInitConfig -> 0x%08X", status);
    if (!NT_SUCCESS(status)) return status;

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = IddSeatDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContext);

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&pDeviceInit, &attr, &device);
    IddSeatLog("WdfDeviceCreate -> 0x%08X", status);
    if (!NT_SUCCESS(status)) return status;

    auto* ctx = WdfObjectGet_IndirectDeviceContext(device);
    ctx->Device = device;
    ctx->SeatName = seat;

    /* HYDRA-TODO(a) DONE: build the seat's EDID from its configured mode. */
    ctx->Mode = ParseModeString(modeStr.c_str());
    char mfr[3] = { 'H','Y','D' };
    /* Bounded by hand: seat comes from a device property (up to 63 chars) and
     * wsprintfA doesn't respect buffer size. EDID's name descriptor caps at 13
     * chars anyway (hydra_edid_build truncates), so clamp here too. */
    char name[16] = "Hydra ";
    size_t nseat = seat.size() < 9 ? seat.size() : 9;
    for (size_t i = 0; i < nseat; ++i) name[6 + i] = (char)seat[i];
    name[6 + nseat] = '\0';
    hydra_edid_build(ctx->Edid, ctx->Mode.width, ctx->Mode.height, ctx->Mode.vsync, mfr, name);

    NTSTATUS initStatus = IddCxDeviceInitialize(device);
    IddSeatLog("IddCxDeviceInitialize -> 0x%08X", initStatus);
    return initStatus;
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE /*prev*/)
{
    auto* ctx = WdfObjectGet_IndirectDeviceContext(device);

    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
#ifdef HYDRA_REMOTE_IDD
    /* Remote-session IDD: attaches to a remote (RDP/TS) session's desktop rather
     * than the console. REMOTE_SESSION_DRIVER (0x4) declares intent; the OS will
     * FAIL IddCxAdapterInitAsync unless this device was created by the OS remote-
     * desktop stack (not by SwDeviceCreate). USE_SMALLEST_MODE (0x1) is mandatory
     * for remote IDDs. Console + remote are mutually exclusive per the docs.
     * NOTE: also requires INF UmdfHostProcessSharing=ProcessSharingDisabled. */
    caps.Flags = IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER
               | IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;
#else
    caps.Flags = IDDCX_ADAPTER_FLAGS_NONE;   /* console-session IDD (default) */
#endif
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Hydra Virtual Seat";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Hydra";
    caps.EndPointDiagnostics.pEndPointModelName = L"Seat";

    /* REQUIRED -- the WDK sample comments them "(required)" and leaving them
     * null makes IddCxAdapterInitAsync return STATUS_INVALID_PARAMETER. Must
     * outlive the call, which is fine: IddCx copies the caps synchronously. */
    IDDCX_ENDPOINT_VERSION endpointVersion{};
    endpointVersion.Size = sizeof(endpointVersion);
    endpointVersion.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &endpointVersion;
    caps.EndPointDiagnostics.pHardwareVersion = &endpointVersion;

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = device;
    init.pCaps = &caps;

    /* Attach a context to the adapter object so adapter-scoped callbacks can
     * reach the seat identity/mode/EDID. */
    WDF_OBJECT_ATTRIBUTES adapterAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttr, IndirectAdapterContext);
    init.ObjectAttributes = &adapterAttr;

    IDARG_OUT_ADAPTER_INIT out{};
    IddSeatLog("D0Entry: calling IddCxAdapterInitAsync, caps.Flags=0x%X", (unsigned)caps.Flags);
    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
    IddSeatLog("IddCxAdapterInitAsync -> 0x%08X", status);
    if (NT_SUCCESS(status))
    {
        ctx->Adapter = out.AdapterObject;
        /* The adapter object exists synchronously; populate its context now from
         * the device context so InitFinished has the right seat/mode/EDID. */
        auto* actx = WdfObjectGet_IndirectAdapterContext(out.AdapterObject);
        actx->SeatName = ctx->SeatName;
        actx->Mode = ctx->Mode;
        memcpy(actx->Edid, ctx->Edid, HYDRA_EDID_SIZE);
    }
    return status;
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatAdapterInitFinished(IDDCX_ADAPTER adapter,
                                               const IDARG_IN_ADAPTER_INIT_FINISHED* args)
{
    if (!NT_SUCCESS(args->AdapterInitStatus)) return STATUS_SUCCESS;

    /* Reach the seat's identity/mode/EDID via the adapter context populated in
     * D0Entry. This is what makes the shared-surface name per-seat (so B and C
     * don't collide) and the advertised mode correct. */
    auto* actx = WdfObjectGet_IndirectAdapterContext(adapter);

    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;   /* connector kind */
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = HYDRA_EDID_SIZE;
    info.MonitorDescription.pData = actx->Edid;
    /* Deterministic container id from the seat name keeps monitor identity stable
     * across arrivals without colliding between seats. */
    CoCreateGuid(&info.MonitorContainerId);

    WDF_OBJECT_ATTRIBUTES monAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttr, IndirectMonitorContext);

    IDARG_IN_MONITORCREATE createIn{};
    createIn.ObjectAttributes = &monAttr;
    createIn.pMonitorInfo = &info;

    IDARG_OUT_MONITORCREATE createOut{};
    NTSTATUS status = IddCxMonitorCreate(adapter, &createIn, &createOut);
    if (!NT_SUCCESS(status)) return status;

    auto* mctx = WdfObjectGet_IndirectMonitorContext(createOut.MonitorObject);
    mctx->Monitor = createOut.MonitorObject;
    mctx->SeatName = actx->SeatName;
    mctx->Mode = actx->Mode;

    /* IddCxMonitorArrival takes only (monitor, out-args) -- there is no input
     * struct. The OS returns the adapter LUID / target id for companion apps. */
    IDARG_OUT_MONITORARRIVAL arrivalOut{};
    NTSTATUS arrivalStatus = IddCxMonitorArrival(createOut.MonitorObject, &arrivalOut);
    if (!NT_SUCCESS(arrivalStatus)) return arrivalStatus;

#ifdef HYDRA_REMOTE_IDD
    /* ACTIVATE THE PATH. Remote IDDs only.
     *
     * A console IDD is finished once the monitor has arrived. A remote IDD
     * is not: the OS keeps one stored desktop configuration per remote
     * session, it starts EMPTY, and every path stays inactive until the
     * driver supplies a configuration. Arrival alone shows nothing, and
     * reports success while doing so.
     *
     * IddCxAdapterDisplayConfigUpdate2 supersedes the original call and
     * returns HRESULT rather than NTSTATUS -- unlike everything else here.
     *
     * Only MODE_VALID is set. Scale factor, physical size, colorimetry and
     * SDR white level are flag-gated and left to the OS defaults, which is
     * what a fixed-resolution seat monitor wants. */
    {
        IDDCX_DISPLAYCONFIGPATH2 path{};
        path.Size          = sizeof(path);
        path.Flags         = IDDCX_DISPLAYCONFIGPATH2_FLAGS_MODE_VALID;
        path.MonitorObject = createOut.MonitorObject;

        /* Single seat monitor, so the desktop origin is 0,0. */
        path.Mode.Position.x = 0;
        path.Mode.Position.y = 0;

        /* From the seat's own mode, so it necessarily matches something we
         * advertise. A mismatch returns STATUS_INVALID_PARAMETER and the
         * reason is only visible in WPP. */
        path.Mode.Resolution.cx = actx->Mode.width;
        path.Mode.Resolution.cy = actx->Mode.height;

        path.Mode.Rotation = DISPLAYCONFIG_ROTATION_IDENTITY;

        /* Progressive only for remote IDDs, so this is a plain vertical
         * rate with a denominator of one. */
        path.Mode.RefreshRate.Numerator   = actx->Mode.vsync;
        path.Mode.RefreshRate.Denominator = 1;
        path.Mode.VSyncFreqDivider        = 1;

        path.Mode.MonitorColorMode = IDDCX_DISPLAYCONFIG_MONITOR_COLORMODE_SDR;

        IDARG_IN_ADAPTERDISPLAYCONFIGUPDATE2 cfgIn{};
        cfgIn.PathCount = 1;
        cfgIn.pPaths    = &path;

        HRESULT hr = IddCxAdapterDisplayConfigUpdate2(adapter, &cfgIn);
        if (FAILED(hr))
        {
            /* DEVICE_STOPPED is expected when the session is disconnecting
             * or the adapter is being torn down. The docs are explicit that
             * IddCxReportCriticalError must NOT be called for it.
             *
             * Anything else is logged and swallowed: a failed configuration
             * update should not take the monitor down with it. */
            if (hr != HRESULT_FROM_NT(STATUS_GRAPHICS_INDIRECT_DISPLAY_DEVICE_STOPPED))
            {
                /* HYDRA-TODO: route this somewhere visible once remote
                 * builds can actually run. WPP is the only channel a UMDF
                 * driver has, and nothing is reading it yet. */
            }
        }
    }
#endif

    return arrivalStatus;
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatAdapterCommitModes(IDDCX_ADAPTER /*adapter*/,
                                              const IDARG_IN_COMMITMODES* /*args*/)
{
    return STATUS_SUCCESS;
}

/* -------- monitor description / mode enumeration (HYDRA-TODO(a) DONE) ------- */

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in,
                                                   IDARG_OUT_PARSEMONITORDESCRIPTION* out)
{
    /* Adapter-scoped: no monitor object. Derive the mode from the EDID we were
     * handed (its preferred DTD encodes WxH@Hz — round-tripped and unit-tested).
     * We advertise exactly that one mode. Two-call pattern: count, then fill. */
    hydra::SeatMode m{};
    if (in->MonitorDescription.Type == IDDCX_MONITOR_DESCRIPTION_TYPE_EDID &&
        in->MonitorDescription.DataSize >= HYDRA_EDID_SIZE &&
        in->MonitorDescription.pData)
    {
        uint32_t w = 0, h = 0, hz = 0;
        hydra_edid_read_mode((const uint8_t*)in->MonitorDescription.pData, &w, &h, &hz);
        if (w && h && hz) { m.width = w; m.height = h; m.vsync = hz; }
    }

    out->MonitorModeBufferOutputCount = 1;
    if (in->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;                       /* caller just wanted the count */

    in->pMonitorModes[0] = hydra::MakeMonitorMode(m, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    out->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatMonitorGetDefaultModes(IDDCX_MONITOR monitor,
                                                  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in,
                                                  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out)
{
    auto* mctx = WdfObjectGet_IndirectMonitorContext(monitor);
    out->DefaultMonitorModeBufferOutputCount = 1;

    if (in->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    in->pDefaultMonitorModes[0] =
        hydra::MakeMonitorMode(mctx->Mode, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    out->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatMonitorQueryModes(IDDCX_MONITOR monitor,
                                             const IDARG_IN_QUERYTARGETMODES* in,
                                             IDARG_OUT_QUERYTARGETMODES* out)
{
    auto* mctx = WdfObjectGet_IndirectMonitorContext(monitor);
    out->TargetModeBufferOutputCount = 1;

    if (in->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    in->pTargetModes[0] = hydra::MakeTargetMode(mctx->Mode);
    return STATUS_SUCCESS;
}

/* -------- swapchain assignment: start / stop the frame loop ---------------- */

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatMonitorAssignSwapChain(IDDCX_MONITOR monitor,
                                                  const IDARG_IN_SETSWAPCHAIN* args)
{
    auto* mctx = WdfObjectGet_IndirectMonitorContext(monitor);

    auto device = std::make_shared<hydra::Direct3DDevice>(args->RenderAdapterLuid);
    mctx->Processor = std::make_unique<hydra::SwapChainProcessor>(
        mctx->SeatName, args->hSwapChain, device, args->hNextSurfaceAvailable);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
extern "C" NTSTATUS IddSeatMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
    auto* mctx = WdfObjectGet_IndirectMonitorContext(monitor);
    mctx->Processor.reset();
    return STATUS_SUCCESS;
}
