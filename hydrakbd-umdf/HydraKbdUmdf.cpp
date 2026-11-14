#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include <ntstatus.h>
#include <wdf.h>
#include <ntddkbd.h>

// Fallback status codes -- ntstatus.h / WIN32_NO_STATUS interplay on this SDK
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL       ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED          ((NTSTATUS)0xC00000BBL)
#endif

static const GUID g_Iface = { 0xa5b1c2d3,0xe4f5,0x6789,{0xab,0xcd,0xef,0x01,0x23,0x45,0x67,0x89} };
#define LOG_PATH L"C:\\HydraLog\\keyboard-umdf.log"

static void L(const char *fmt, ...) {
    HANDLE h = CreateFileW(LOG_PATH, FILE_APPEND_DATA,
        FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    char buf[1024];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%llu ", u.QuadPart);
    if (n<0) n=0;
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf_s(buf+n, sizeof(buf)-n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m>0) n+=m;
    if (n<(int)sizeof(buf)-2) { buf[n++]='\r'; buf[n++]='\n'; }
    DWORD w; WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

typedef struct _DC { WDFQUEUE rq; } DC, *PDC;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DC, KbdGetDC)
typedef struct _FC { BOOLEAN c; } FC, *PFC;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FC, KbdGetFC)

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;
EVT_WDF_DEVICE_FILE_CREATE EvtDeviceFileCreate;
EVT_WDF_FILE_CLEANUP EvtFileCleanup;
EVT_WDF_FILE_CLOSE EvtFileClose;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_READ EvtIoRead;

NTSTATUS EvtDriverDeviceAdd(WDFDRIVER D, PWDFDEVICE_INIT DI) {
    NTSTATUS s; WDFDEVICE dev;
    WDF_OBJECT_ATTRIBUTES a, fa;
    WDF_FILEOBJECT_CONFIG fc;
    WDF_IO_QUEUE_CONFIG qc;
    WDFQUEUE q;

    L("DeviceAdd: entered");
    WdfDeviceInitSetIoType(DI, WdfDeviceIoBuffered);


    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fa, FC);
    WDF_FILEOBJECT_CONFIG_INIT(&fc, EvtDeviceFileCreate, EvtFileClose, EvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DI, &fc, &fa);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, DC);
    s = WdfDeviceCreate(&DI, &a, &dev);
    if (!NT_SUCCESS(s)) { L("DeviceCreate failed 0x%08x", s); return s; }

    s = WdfDeviceCreateDeviceInterface(dev, &g_Iface, NULL);
    if (!NT_SUCCESS(s)) { L("CreateDeviceInterface failed 0x%08x", s); return s; }

    WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual);
    s = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, &q);
    if (!NT_SUCCESS(s)) { L("read queue failed 0x%08x", s); return s; }
    KbdGetDC(dev)->rq = q;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoDeviceControl = EvtIoDeviceControl;
    qc.EvtIoRead = EvtIoRead;
    s = WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, &q);
    if (!NT_SUCCESS(s)) { L("default queue failed 0x%08x", s); return s; }

    L("DeviceAdd: created");
    return STATUS_SUCCESS;
}

VOID EvtDeviceFileCreate(WDFDEVICE D, WDFREQUEST R, WDFFILEOBJECT F) {
    UNREFERENCED_PARAMETER(D);
    L("FileCreate: file=%p", F);
    WdfRequestComplete(R, STATUS_SUCCESS);
}

VOID EvtFileCleanup(WDFFILEOBJECT F) {
    PFC c = KbdGetFC(F);
    if (c->c) return;
    c->c = TRUE;
    L("FileCleanup: file=%p", F);
}

VOID EvtFileClose(WDFFILEOBJECT F) {
    L("FileClose: file=%p", F);
}

VOID EvtIoRead(WDFQUEUE Q, WDFREQUEST R, size_t Len) {
    PDC c = KbdGetDC(WdfIoQueueGetDevice(Q));
    L("READ: len=%zu -> pending", Len);
    NTSTATUS s = WdfRequestForwardToIoQueue(R, c->rq);
    if (!NT_SUCCESS(s)) { L("  forward failed 0x%08x", s); WdfRequestComplete(R, s); }
}

VOID EvtIoDeviceControl(WDFQUEUE Q, WDFREQUEST R,
                        size_t OB, size_t IB, ULONG Code) {
    NTSTATUS s = STATUS_SUCCESS;
    size_t info = 0;
    PVOID ob = NULL; size_t ol = 0;
    UNREFERENCED_PARAMETER(Q);
    UNREFERENCED_PARAMETER(IB);

    if (OB > 0) WdfRequestRetrieveOutputBuffer(R, OB, &ob, &ol);
    L("IOCTL: 0x%08x out=%zu", Code, OB);

    switch (Code) {
    case IOCTL_KEYBOARD_QUERY_ATTRIBUTES:
        if (ob == NULL || ol < sizeof(KEYBOARD_ATTRIBUTES)) {
            s = STATUS_BUFFER_TOO_SMALL;
            L("  QUERY_ATTR: buffer too small");
            break;
        }
        {
            PKEYBOARD_ATTRIBUTES a = (PKEYBOARD_ATTRIBUTES)ob;
            RtlZeroMemory(a, sizeof(*a));
            a->KeyboardIdentifier.Type    = 4;
            a->KeyboardIdentifier.Subtype = 0;
            a->KeyboardMode               = 1;
            a->NumberOfFunctionKeys       = 12;
            a->NumberOfIndicators         = 3;
            a->NumberOfKeysTotal          = 101;
            a->InputDataQueueLength       = 32;
            a->KeyRepeatMinimum.UnitId    = 0;
            a->KeyRepeatMinimum.Rate      = 2;
            a->KeyRepeatMinimum.Delay     = 250;
            a->KeyRepeatMaximum.UnitId    = 0;
            a->KeyRepeatMaximum.Rate      = 30;
            a->KeyRepeatMaximum.Delay     = 1000;
            info = sizeof(*a);
        }
        L("  QUERY_ATTR: SUCCESS %zu bytes", info);
        break;

    case IOCTL_KEYBOARD_SET_TYPEMATIC:
        L("  SET_TYPEMATIC: SUCCESS");
        break;
    case IOCTL_KEYBOARD_SET_INDICATORS:
        L("  SET_INDICATORS: SUCCESS");
        break;

    case IOCTL_KEYBOARD_QUERY_TYPEMATIC:
        if (ob == NULL || ol < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS)) { s = STATUS_BUFFER_TOO_SMALL; break; }
        {
            PKEYBOARD_TYPEMATIC_PARAMETERS p = (PKEYBOARD_TYPEMATIC_PARAMETERS)ob;
            p->UnitId = 0; p->Rate = 2; p->Delay = 250;
            info = sizeof(*p);
        }
        L("  QUERY_TYPEMATIC: SUCCESS");
        break;

    case IOCTL_KEYBOARD_QUERY_INDICATORS:
        if (ob == NULL || ol < sizeof(KEYBOARD_INDICATOR_PARAMETERS)) { s = STATUS_BUFFER_TOO_SMALL; break; }
        {
            PKEYBOARD_INDICATOR_PARAMETERS p = (PKEYBOARD_INDICATOR_PARAMETERS)ob;
            p->UnitId = 0; p->LedFlags = 0;
            info = sizeof(*p);
        }
        L("  QUERY_INDICATORS: SUCCESS");
        break;

    case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION:
        if (ob == NULL || ol < sizeof(USHORT)) { s = STATUS_BUFFER_TOO_SMALL; break; }
        RtlZeroMemory(ob, ol);
        info = sizeof(USHORT);
        L("  QUERY_TRANSLATION: SUCCESS");
        break;

    case IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES:
    case 0xB1000:
    case 0xB1004:
        s = STATUS_NOT_SUPPORTED;
        L("  0x%08x -> NOT_SUPPORTED", Code);
        break;

    default:
        L("  unknown 0x%08x -> NOT_SUPPORTED", Code);
        s = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(R, s, info);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT D, PUNICODE_STRING R) {
    WDF_DRIVER_CONFIG c;
    NTSTATUS s;
    L("DriverEntry: HydraKbdUmdf B17");
    WDF_DRIVER_CONFIG_INIT(&c, EvtDriverDeviceAdd);
    s = WdfDriverCreate(D, R, WDF_NO_OBJECT_ATTRIBUTES, &c, WDF_NO_HANDLE);
    if (!NT_SUCCESS(s)) { L("WdfDriverCreate failed 0x%08x", s); return s; }
    L("DriverEntry: SUCCESS");
    return STATUS_SUCCESS;
}
