/* iddseat.h  --  Hydra virtual display (IddCx indirect display driver)
 *
 * WHAT THIS IS
 *   A UMDF (user-mode) Indirect Display Driver on the IddCx class extension. One
 *   virtual monitor per instance. Windows renders a desktop into that monitor and
 *   hands each frame back as a Direct3D texture; the SwapChainProcessor copies it
 *   into a *named, cross-session* shared texture that the per-seat `mirror`
 *   process opens by name and presents on a real panel. No RDP on the display
 *   path.
 *
 * WHAT THIS IS NOT
 *   - Not kernel-mode. IddCx drivers run in Wudfhost.exe. No ring-0 code, none
 *     possible with this model (a ring-0 display driver is a WDDM miniport = GPU
 *     vendor territory, permanently out of scope).
 *   - Not compiled in the dev container: this is a WDK + MSVC + D3D11/DXGI
 *     artifact and there is no MinGW/Linux path for IddCx. Build on Windows with
 *     the WDK; development requires test-signing.
 *
 * STATE OF THE THREE ORIGINAL FILL-INS
 *   (a) EDID / mode enumeration  -- DONE. Uses common/hydra_edid.h to synthesise
 *       a checksum-valid EDID from the seat's WxH@Hz, and the mode callbacks
 *       advertise exactly that mode. (The EDID generator is unit-tested natively.)
 *   (b) shared-surface IPC       -- DONE. SharedTextureSink below implements the
 *       producer half of common/hydra_ipc.h: named Global\ keyed-mutex texture +
 *       a metadata section. No cross-session handle duplication.
 *   (c) render-adapter LUID      -- DONE. The LUID IddCx reports for the swapchain
 *       is used to build our consuming device AND is published in the metadata so
 *       the mirror can match it. hydrad still owns GPU *affinity* policy.
 *   Seat name is read from a device property set by hydrad at SwDeviceCreate.
 *
 * The one genuinely hard integration point that is NOT solved by any single file
 * is binding a virtual monitor to a *specific TS session's* desktop -- that is
 * hydrad's job and ARCHITECTURE.md risk #1. This driver just makes the monitor.
 */

#pragma once

/* NTSTATUS handling for a UMDF driver that also uses WRL + Win32:
 * windows.h normally defines a *subset* of NTSTATUS codes, which then collide
 * with ntstatus.h (used by wdf/iddcx). The canonical fix is to tell windows.h to
 * stay out of the STATUS_* business (WIN32_NO_STATUS) around its include, then
 * pull the full set from ntstatus.h explicitly. Without this, WRL's
 * corewrappers.h fails on STATUS_WAIT_0 and cascades. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>

/* IddCx's function-enum header requires the target IddCx version be declared
 * before <iddcx.h> is pulled in, or it errors with
 * "IDDCX_VERSION_MAJOR is not defined". We target IddCx 1.11 (major 1, minor 11)
 * -- a 1.11 binary runs on Windows 10 1803+ via runtime feature checks. */
#ifndef IDDCX_VERSION_MAJOR
#define IDDCX_VERSION_MAJOR 1
#endif
#ifndef IDDCX_VERSION_MINOR
#define IDDCX_VERSION_MINOR 11
#endif
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <string>

#include "../common/hydra_edid.h"
#include "../common/hydra_ipc.h"

namespace hydra
{
    /* A per-seat desired mode, parsed from seats.toml `edid = "WxH@Hz"`. */
    struct SeatMode { UINT width = 1920; UINT height = 1080; UINT vsync = 60; };

    /* Direct3D device bound to a specific adapter LUID (the one IddCx reports for
     * the swapchain). Same-LUID consuming device = zero-copy path to the mirror
     * *if* the mirror also runs on that GPU; hydrad pins affinity to make it so. */
    struct Direct3DDevice
    {
        Direct3DDevice() = default;
        explicit Direct3DDevice(LUID AdapterLuid);
        HRESULT Init();

        LUID                                        AdapterLuid{};
        Microsoft::WRL::ComPtr<IDXGIFactory5>       DxgiFactory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1>       Adapter;
        Microsoft::WRL::ComPtr<ID3D11Device>        Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
    };

    /* Producer half of hydra_ipc.h. Owns the named metadata section and the named
     * shared keyed-mutex texture; copies each acquired frame into the shared
     * texture under key 0. Created lazily on the first frame (needs the frame's
     * dimensions/format). */
    class IMirrorSink
    {
    public:
        virtual ~IMirrorSink() = default;
        virtual HRESULT Publish(ID3D11Texture2D* frame) = 0;
        virtual void    Retire() = 0;
    };

    std::unique_ptr<IMirrorSink> MakeSharedTextureSink(
        const wchar_t* seatName, const Direct3DDevice& device);

    /* SwapChainProcessor -- owns one monitor's frame loop. */
    class SwapChainProcessor
    {
    public:
        SwapChainProcessor(std::wstring seatName,
                           IDDCX_SWAPCHAIN hSwapChain,
                           std::shared_ptr<Direct3DDevice> device,
                           HANDLE newFrameEvent);
        ~SwapChainProcessor();

    private:
        void Run();
        void RunCore();

        std::wstring                    m_seatName;
        IDDCX_SWAPCHAIN                 m_hSwapChain;
        std::shared_ptr<Direct3DDevice> m_device;
        HANDLE                          m_newFrameEvent;
        Microsoft::WRL::Wrappers::Event m_terminateEvent;
        std::unique_ptr<IMirrorSink>    m_sink;
        std::thread                     m_thread;
    };

    /* Fill a DISPLAYCONFIG_VIDEO_SIGNAL_INFO / IDDCX_MONITOR_MODE from WxH@Hz. */
    IDDCX_MONITOR_MODE MakeMonitorMode(const SeatMode& m, IDDCX_MONITOR_MODE_ORIGIN origin);
    IDDCX_TARGET_MODE  MakeTargetMode(const SeatMode& m);
}

/* ---- WDF/IddCx contexts -------------------------------------------------- */

struct IndirectDeviceContext
{
    WDFDEVICE          Device{};
    IDDCX_ADAPTER      Adapter{};
    std::wstring       SeatName;                 /* from device property; default "B" */
    hydra::SeatMode    Mode;                     /* parsed from the seat's EDID string */
    BYTE               Edid[HYDRA_EDID_SIZE];    /* built once, referenced by monitor  */
};
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContext);

/* Carried on the IDDCX_ADAPTER object so the adapter-scoped callbacks (which get
 * an adapter, not the WDFDEVICE) can reach the seat's identity/mode/EDID. Filled
 * in D0Entry immediately after IddCxAdapterInitAsync returns the adapter. */
struct IndirectAdapterContext
{
    std::wstring    SeatName;
    hydra::SeatMode Mode;
    BYTE            Edid[HYDRA_EDID_SIZE];
};
WDF_DECLARE_CONTEXT_TYPE(IndirectAdapterContext);

struct IndirectMonitorContext
{
    IDDCX_MONITOR                              Monitor{};
    std::wstring                               SeatName;
    hydra::SeatMode                            Mode;
    std::unique_ptr<hydra::SwapChainProcessor> Processor;
};
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContext);

extern "C" {
    DRIVER_INITIALIZE                 DriverEntry;
    EVT_WDF_DRIVER_DEVICE_ADD         IddSeatDeviceAdd;
    EVT_WDF_DEVICE_D0_ENTRY           IddSeatDeviceD0Entry;

    EVT_IDD_CX_ADAPTER_INIT_FINISHED   IddSeatAdapterInitFinished;
    EVT_IDD_CX_ADAPTER_COMMIT_MODES    IddSeatAdapterCommitModes;

    EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION             IddSeatParseMonitorDescription;
    EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES IddSeatMonitorGetDefaultModes;
    EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES            IddSeatMonitorQueryModes;
    EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN              IddSeatMonitorAssignSwapChain;
    EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN            IddSeatMonitorUnassignSwapChain;
}
