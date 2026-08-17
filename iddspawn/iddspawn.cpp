/* iddspawn.cpp -- create the seat's virtual-display devnode from an INTERACTIVE
 * session, and hold it open.
 *
 * WHY THIS EXISTS
 *   2026-08-17. Microsoft's own IddSampleDriver, built with build-driver.ps1's
 *   exact recipe and signed with our cert, LOADS on
 *
 *       SWD\IddSampleDriver\IddSampleDriver     (created by IddSampleApp.exe)
 *
 *   and FAILS with 0xD000000D on
 *
 *       SWD\Hydra\HydraSeat_B                   (created by hydrad)
 *
 *   Same binary. Same package. Same machine. So the fault was never in
 *   iddseat.cpp -- it is in how the devnode is created. Twelve other candidates
 *   were eliminated (see the 08-17 commit): the two DEVPROPERTY entries, the
 *   Root\ hardware-id prefix, pszzCompatibleIds, the class instance key,
 *   UmdfExtensions absent/0110/0102, the IndirectKmd upper filter, ClassVer,
 *   ServiceBinary %13% vs %12%\UMDF, UMDF 2.35/2.33, IddCx 1.11/1.10/1.2, and
 *   IDDCX_VERSION_MAJOR/MINOR. A registry diff of the two devnodes shows
 *   identical UpperFilters, DeviceGroupId, DriverList, KernelModeClientPolicy
 *   and Capabilities.
 *
 *   The one structural difference left: IddSampleApp.exe runs interactively in
 *   session 1; hydrad is a service in session 0.
 *
 *   This is that difference isolated. Run it from the console session. If the
 *   device loads here and not under hydrad, session-0 creation is the cause and
 *   the fix is to have hydrad supervise this helper in the seat's session --
 *   exactly as it already supervises mirror and clip.
 *
 * LIFETIME
 *   A SwDevice lives only as long as the process that created it. This must
 *   keep running. Ctrl+C removes the device cleanly.
 *
 * BUILD (x64 Native Tools)
 *   cl /O2 /EHsc /std:c++17 /nologo iddspawn\iddspawn.cpp /Fe:dist\iddspawn.exe onecore.lib
 *
 * USAGE
 *   .\dist\iddspawn.exe                      seat B, 1920x1080@60
 *   .\dist\iddspawn.exe B 1920x1080@60
 *   .\dist\iddspawn.exe B 1920x1080@60 -noprops
 *
 *   -noprops omits the seat-name/seat-mode device properties. Microsoft's
 *   sample driver ignores them; iddseat.cpp reads them in DeviceAdd. Kept as a
 *   switch so the two can be compared without another rebuild.
 */

#include <windows.h>
#include <swdevice.h>
#include <string>
#include <cstdio>

#pragma comment(lib, "onecore.lib")

/* Must match hydrad.cpp. The enumerator name becomes SWD\<this>\<instance>. */
#define SVC_NAME  L"Hydra"

/* Bare, no "Root\" prefix: that form is for root-enumerated installs, and
 * iddseat.inf now lists both so either matches. */
#define HW_ID     L"HydraSeat"

#include "../common/hydra_devprops.h"

struct SwWait { HANDLE done; HRESULT hr; };

static VOID WINAPI create_cb(HSWDEVICE, HRESULT hr, PVOID ctx, PCWSTR)
{
    auto* w = (SwWait*)ctx;
    w->hr = hr;
    SetEvent(w->done);
}

static volatile bool g_stop = false;
static BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

int wmain(int argc, wchar_t** argv)
{
    std::wstring seat = (argc > 1) ? argv[1] : L"B";
    std::wstring mode = (argc > 2) ? argv[2] : L"1920x1080@60";
    bool noProps = false;
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"-noprops") == 0) noProps = true;

    std::wstring instanceId = L"HydraSeat_" + seat;
    static const wchar_t hwids[] = HW_ID L"\0";

    SetConsoleCtrlHandler(on_ctrl, TRUE);

    wprintf(L"[iddspawn] session %lu, creating SWD\\%s\\%s  hwid=%s  props=%s\n",
            WTSGetActiveConsoleSessionId(), SVC_NAME, instanceId.c_str(),
            HW_ID, noProps ? L"no" : L"yes");

    SW_DEVICE_CREATE_INFO ci{};
    ci.cbSize            = sizeof(ci);
    ci.pszInstanceId     = instanceId.c_str();
    ci.pszzHardwareIds   = hwids;
    ci.pszzCompatibleIds = nullptr;   /* MS's sample sets none */
    ci.pszDeviceDescription = L"Hydra Virtual Seat Display";
    ci.CapabilityFlags   = SWDeviceCapabilitiesRemovable
                         | SWDeviceCapabilitiesSilentInstall
                         | SWDeviceCapabilitiesDriverRequired;

    DEVPROPERTY props[2]{};
    props[0].CompKey.Key   = DEVPKEY_Hydra_SeatName;
    props[0].CompKey.Store = DEVPROP_STORE_SYSTEM;
    props[0].Type          = DEVPROP_TYPE_STRING;
    props[0].Buffer        = (PVOID)seat.c_str();
    props[0].BufferSize    = (ULONG)((seat.size() + 1) * sizeof(wchar_t));

    props[1].CompKey.Key   = DEVPKEY_Hydra_SeatMode;
    props[1].CompKey.Store = DEVPROP_STORE_SYSTEM;
    props[1].Type          = DEVPROP_TYPE_STRING;
    props[1].Buffer        = (PVOID)mode.c_str();
    props[1].BufferSize    = (ULONG)((mode.size() + 1) * sizeof(wchar_t));

    SwWait wait{ CreateEventW(nullptr, TRUE, FALSE, nullptr), E_FAIL };
    HSWDEVICE h = nullptr;

    HRESULT hr = SwDeviceCreate(SVC_NAME, L"HTREE\\ROOT\\0", &ci,
                                noProps ? 0 : 2, noProps ? nullptr : props,
                                create_cb, &wait, &h);
    if (FAILED(hr)) {
        wprintf(L"[iddspawn] SwDeviceCreate FAILED hr=0x%08lX\n", hr);
        return 1;
    }

    if (WaitForSingleObject(wait.done, 15000) != WAIT_OBJECT_0) {
        wprintf(L"[iddspawn] create callback did not fire within 15s\n");
        SwDeviceClose(h);
        return 2;
    }
    CloseHandle(wait.done);

    if (FAILED(wait.hr)) {
        /* This fires for install-time failures. A driver that installs but then
         * refuses to LOAD reports success here and shows up as
         * CM_PROB_FAILED_ADD on the devnode instead -- check Get-PnpDevice. */
        wprintf(L"[iddspawn] create callback hr=0x%08lX "
                L"(driver staged and test-signed?)\n", wait.hr);
        SwDeviceClose(h);
        return 3;
    }

    wprintf(L"[iddspawn] device created (%s). Holding it open.\n", mode.c_str());
    wprintf(L"[iddspawn] Now check, from another window:\n");
    wprintf(L"[iddspawn]   Get-PnpDevice | ? InstanceId -match 'HYDRA' | "
            L"Select InstanceId, Status, Problem\n");
    wprintf(L"[iddspawn] Status OK = session-0 creation was the bug.\n");
    wprintf(L"[iddspawn] Ctrl+C to remove the device.\n");

    while (!g_stop) Sleep(200);

    wprintf(L"[iddspawn] closing\n");
    SwDeviceClose(h);
    return 0;
}
