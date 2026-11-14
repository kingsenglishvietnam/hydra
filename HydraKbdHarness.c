// HydraKbdHarness.c -- Phase 3c/3d harness for HydraKbdUmdf B17.
// Opens the private-interface device, issues 0xb0000, holds the handle
// open 500 ms, prints KEYBOARD_ATTRIBUTES. Runs as anyone with access.
#include <windows.h>
#include <stdio.h>
#include <setupapi.h>

// {a5b1c2d3-e4f5-6789-abcd-ef0123456789}
static const GUID GUID_DEVINTERFACE_HYDRAKBD =
    { 0xa5b1c2d3, 0xe4f5, 0x6789, { 0xab,0xcd,0xef,0x01,0x23,0x45,0x67,0x89 } };

#define IOCTL_QUERY_ATTRIBUTES 0xb0000

typedef struct { UCHAR Type; UCHAR Subtype; } KEYBOARD_ID;
typedef struct { USHORT UnitId; USHORT Rate; USHORT Delay; } KEYBOARD_TYPEMATIC;
typedef struct {
    KEYBOARD_ID        KeyboardIdentifier;
    USHORT             KeyboardMode;
    USHORT             NumberOfFunctionKeys;
    USHORT             NumberOfIndicators;
    USHORT             NumberOfKeysTotal;
    ULONG              InputDataQueueLength;
    KEYBOARD_TYPEMATIC KeyRepeatMinimum;
    KEYBOARD_TYPEMATIC KeyRepeatMaximum;
} KEYBOARD_ATTRIBUTES;

int main(void) {
    HDEVINFO hdi = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_HYDRAKBD, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs: %lu\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(hdi, NULL, &GUID_DEVINTERFACE_HYDRAKBD, 0, &ifd)) {
        printf("SetupDiEnumDeviceInterfaces: %lu\n", GetLastError());
        return 1;
    }

    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(hdi, &ifd, NULL, 0, &need, NULL);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
    det->cbSize = sizeof(*det);
    if (!SetupDiGetDeviceInterfaceDetailW(hdi, &ifd, det, need, NULL, NULL)) {
        printf("SetupDiGetDeviceInterfaceDetail: %lu\n", GetLastError());
        return 1;
    }
    printf("Path: %ws\n", det->DevicePath);

    HANDLE h = CreateFileW(det->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("CreateFile: %lu\n", GetLastError());
        return 1;
    }
    printf("CreateFile: SUCCESS\n");

    KEYBOARD_ATTRIBUTES a = {0};
    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_QUERY_ATTRIBUTES,
                         NULL, 0, &a, sizeof(a), &br, NULL)) {
        printf("DeviceIoControl 0xb0000: %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    printf("0xb0000: SUCCESS, %lu bytes\n", br);
    printf("  Type=%u Subtype=%u Mode=%u FuncKeys=%u Ind=%u Keys=%u QLen=%u\n",
           a.KeyboardIdentifier.Type, a.KeyboardIdentifier.Subtype,
           a.KeyboardMode, a.NumberOfFunctionKeys, a.NumberOfIndicators,
           a.NumberOfKeysTotal, a.InputDataQueueLength);
    printf("  RepMin Rate=%u Delay=%u  RepMax Rate=%u Delay=%u\n",
           a.KeyRepeatMinimum.Rate, a.KeyRepeatMinimum.Delay,
           a.KeyRepeatMaximum.Rate, a.KeyRepeatMaximum.Delay);

    printf("Holding open 500 ms...\n");
    Sleep(500);
    CloseHandle(h);
    printf("Closed.\n");
    return 0;
}