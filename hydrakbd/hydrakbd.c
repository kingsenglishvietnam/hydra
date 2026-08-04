/*++

hydrakbd.c -- Hydra keyboard upper filter driver.

PURPOSE
    Replace the Interception library with our own input capture. Interception is
    dual-licensed: LGPL for NON-COMMERCIAL use only, with commercial use
    requiring a paid licence from its author. That gates Hydra's commercial
    rights on a third party. This driver removes that dependency.

    What Interception actually provides is narrow: a kernel upper-filter on the
    keyboard and mouse class stacks that can SWALLOW input before it reaches the
    normal stack, plus injection. The swallowing is the essential part -- without
    it a seat's keystrokes also reach the console session.

    Its real value is that it ships PRE-SIGNED drivers. That is the thing we are
    taking on ourselves.

=============================================================================
PHASE 1 -- THIS FILE. ATTACH AND OBSERVE ONLY. NOTHING IS EVER BLOCKED.
=============================================================================

    A broken keyboard filter is boot-critical: you can end up with no keyboard at
    the login screen, on a machine you cannot type into to fix it. So phase 1
    does the least dangerous useful thing:

        - attach as an upper filter to every keyboard the class driver sees
        - read and log each device's hardware ID
        - forward EVERY keystroke onward, untouched

    If this driver is completely broken, the worst case is that it fails to load
    and the keyboard behaves exactly as it does today.

    Only once install + signing + attach are proven does phase 2 add selective
    blocking, and phase 3 the user-mode channel.

BEFORE YOU INSTALL, HAVE A WAY BACK:
    - test-signing must already be on (it is, for iddseat)
    - know how to reach Safe Mode: Settings > Recovery > Advanced startup, or
      hold Shift while clicking Restart. Filter drivers are skipped there.
    - a USB keyboard that is NOT one of the filtered devices is a cheap
      insurance policy
    - Last Known Good (F8 equivalent) reverts a driver that stops boot

REMOVAL, if it misbehaves:
    pnputil /enum-drivers | Select-String "hydrakbd"
    pnputil /delete-driver oemNN.inf /uninstall
    ...then reboot. Because this is an UPPER filter, removing it leaves the
    stock keyboard stack intact.

DESIGN NOTES
    KMDF filter driver, so WdfFdoInitSetFilter() puts us in the stack without
    owning the device. Reads are pended by the class driver below us; we set a
    completion routine on IRP_MJ_INTERNAL_DEVICE_CONTROL /
    IOCTL_INTERNAL_KEYBOARD_CONNECT so we see the callback the class driver uses
    to deliver scan codes, which is where phase 2 will filter.

    The service-callback hook is the standard kbfiltr pattern: we substitute our
    own KEYBOARD_INPUT_DATA callback and remember the real one. In phase 1 our
    callback simply forwards to the real one with the count unchanged.

BUILD: WDK, KMDF 1.33+. See build-kbfilter.ps1.
       This CANNOT be cross-checked outside a WDK environment -- kernel headers
       have no MinGW equivalent -- so the first real compile is on your machine.

--*/

#include <ntddk.h>
#include <wdf.h>
#include <kbdmou.h>
#include <ntddkbd.h>
#include <ntdd8042.h>

#define HYDRA_POOL_TAG 'dbkH'   /* "Hkbd" */

/* ------------------------------------------------------------------------- */
/* Per-device context                                                         */
/* ------------------------------------------------------------------------- */

typedef struct _HYDRA_KBD_CONTEXT {

    WDFDEVICE   Device;

    /* The class driver's real callback, and its context. We stand in front of
     * these: it hands us a connect request naming its own callback, we store it
     * here and give it ours instead. Phase 2 filters inside our callback and
     * calls through to this one with a possibly-reduced count. */
    CONNECT_DATA UpperConnectData;

    /* Hardware ID of this keyboard, e.g. "HID\VID_1EA7&PID_0066&...".
     * Phase 2 matches against seats.toml on this. Kept as a counted string so
     * we never depend on NUL termination from the property query. */
    WCHAR       HardwareId[256];
    USHORT      HardwareIdLen;

    /* Phase 1 diagnostics: how many keystrokes have passed through. */
    ULONG64     KeysSeen;

} HYDRA_KBD_CONTEXT, *PHYDRA_KBD_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HYDRA_KBD_CONTEXT, HydraKbdGetContext)

/* ------------------------------------------------------------------------- */

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           HydraKbdEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL HydraKbdEvtIoInternalDeviceControl;

static VOID HydraKbdServiceCallback(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart,
    _In_ PKEYBOARD_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, HydraKbdEvtDeviceAdd)
#endif

/* ------------------------------------------------------------------------- */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, HydraKbdEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    DbgPrint("[hydrakbd] DriverEntry status=0x%08X\n", status);
    return status;
}

/* Read this device's hardware ID so phase 2 can match seats against it.
 * Failure is non-fatal: we still attach and pass through, we just can't
 * identify the device. */
static VOID
HydraKbdCacheHardwareId(
    _In_ WDFDEVICE Device,
    _Inout_ PHYDRA_KBD_CONTEXT Ctx
    )
{
    NTSTATUS status;
    ULONG    required = 0;

    Ctx->HardwareId[0]  = L'\0';
    Ctx->HardwareIdLen  = 0;

    status = WdfDeviceQueryProperty(Device,
                                    DevicePropertyHardwareID,
                                    sizeof(Ctx->HardwareId) - sizeof(WCHAR),
                                    Ctx->HardwareId,
                                    &required);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[hydrakbd] hardware id query failed 0x%08X (need %u)\n",
                 status, required);
        return;
    }

    /* DevicePropertyHardwareID is a REG_MULTI_SZ; the first string is the most
     * specific ID, which is the one we want. Force-terminate defensively. */
    Ctx->HardwareId[(sizeof(Ctx->HardwareId) / sizeof(WCHAR)) - 1] = L'\0';
    Ctx->HardwareIdLen = (USHORT)wcslen(Ctx->HardwareId);

    DbgPrint("[hydrakbd] attached to keyboard: %ws\n", Ctx->HardwareId);
}

NTSTATUS
HydraKbdEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES   attributes;
    WDFDEVICE               device;
    WDF_IO_QUEUE_CONFIG     ioQueueConfig;
    PHYDRA_KBD_CONTEXT      ctx;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    /* FILTER, not owner: we sit above the keyboard class driver and pass
     * everything we don't explicitly handle straight down. This is what makes
     * the driver safe to remove -- the stock stack underneath is untouched. */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, HYDRA_KBD_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[hydrakbd] WdfDeviceCreate failed 0x%08X\n", status);
        return status;
    }

    ctx = HydraKbdGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device = device;

    HydraKbdCacheHardwareId(device, ctx);

    /* Parallel queue: the class driver's connect IOCTL and the pended reads can
     * be in flight together, and we must not serialise them. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchParallel);
    ioQueueConfig.EvtIoInternalDeviceControl = HydraKbdEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device, &ioQueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[hydrakbd] WdfIoQueueCreate failed 0x%08X\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

/*
 * The class driver sends IOCTL_INTERNAL_KEYBOARD_CONNECT once, carrying the
 * callback it wants the port driver to invoke for every batch of scan codes.
 * We intercept that: remember its callback, substitute ours, and forward the
 * request down. From then on every keystroke arrives at our function first.
 *
 * This is the standard kbfiltr pattern and the only hook needed -- there is no
 * separate "keystroke IRP" to intercept.
 */
VOID
HydraKbdEvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    WDFDEVICE           device;
    PHYDRA_KBD_CONTEXT  ctx;
    PCONNECT_DATA       connectData = NULL;
    size_t              length;
    NTSTATUS            status = STATUS_SUCCESS;
    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN             sent;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    device = WdfIoQueueGetDevice(Queue);
    ctx    = HydraKbdGetContext(device);

    switch (IoControlCode) {

    case IOCTL_INTERNAL_KEYBOARD_CONNECT:

        /* Only one connect per device; a second means something is wrong. */
        if (ctx->UpperConnectData.ClassService != NULL) {
            status = STATUS_SHARING_VIOLATION;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CONNECT_DATA),
                                               (PVOID*)&connectData, &length);
        if (!NT_SUCCESS(status)) {
            DbgPrint("[hydrakbd] retrieve connect buffer failed 0x%08X\n", status);
            break;
        }

        ctx->UpperConnectData = *connectData;

        /* Substitute ourselves. The class driver above will now be called by
         * US, not by the port driver directly. */
        connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(device);
        connectData->ClassService      = HydraKbdServiceCallback;

        DbgPrint("[hydrakbd] hooked service callback for %ws\n",
                 ctx->HardwareIdLen ? ctx->HardwareId : L"(unknown device)");
        break;

    case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
        /* Not supported by the class driver either; kept explicit so the
         * intent is visible rather than silently falling through. */
        status = STATUS_NOT_IMPLEMENTED;
        break;

    default:
        break;
    }

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    /* Forward everything downward. We are a filter: anything we don't
     * deliberately handle must reach the real driver unchanged. */
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    sent = WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options);
    if (!sent) {
        status = WdfRequestGetStatus(Request);
        DbgPrint("[hydrakbd] WdfRequestSend failed 0x%08X\n", status);
        WdfRequestComplete(Request, status);
    }
}

/*
 * Every batch of scan codes lands here before the class driver sees it.
 *
 * PHASE 1: pass everything through, unmodified. The ONLY thing this does beyond
 * forwarding is count keystrokes, so we can prove the hook is live without
 * risking the machine's keyboard.
 *
 * PHASE 2 will, for a device whose hardware ID matches a configured seat:
 *   - copy the scan codes into a ring buffer for the user-mode agent
 *   - call the real callback with InputDataStart == InputDataEnd, i.e. deliver
 *     NOTHING upward, which is what makes the key invisible to the console
 *     session. That single line is the whole of what Interception provides.
 *
 * Runs at IRQL <= DISPATCH_LEVEL: no paging, no blocking, no allocation.
 */
VOID
HydraKbdServiceCallback(
    _In_ PDEVICE_OBJECT       DeviceObject,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart,
    _In_ PKEYBOARD_INPUT_DATA InputDataEnd,
    _Inout_ PULONG            InputDataConsumed
    )
{
    WDFDEVICE          device;
    PHYDRA_KBD_CONTEXT ctx;

    device = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
    ctx    = HydraKbdGetContext(device);

    ctx->KeysSeen += (ULONG64)(InputDataEnd - InputDataStart);

    /* Heartbeat every 64 keystrokes -- enough to confirm the hook is live in
     * DebugView without flooding it. */
    if ((ctx->KeysSeen & 0x3F) == 0) {
        DbgPrint("[hydrakbd] %ws: %llu keys seen (passthrough)\n",
                 ctx->HardwareIdLen ? ctx->HardwareId : L"(unknown)",
                 ctx->KeysSeen);
    }

    /* PASSTHROUGH. Phase 2 replaces this with a filtering decision. */
    (*(PSERVICE_CALLBACK_ROUTINE)(ULONG_PTR)ctx->UpperConnectData.ClassService)(
        ctx->UpperConnectData.ClassDeviceObject,
        InputDataStart,
        InputDataEnd,
        InputDataConsumed);
}
