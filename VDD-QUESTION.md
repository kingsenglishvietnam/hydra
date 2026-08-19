# IddCx driver fails 0xD000000D at UMDF load level 0 — while MttVDD loads fine on the same machine

**Windows 11 24H2, build 26100.8972. WDK/SDK 10.0.26100.0, VS 2022 Build Tools
17.14. Test-signing on, self-signed cert trusted in both LocalMachine\Root and
TrustedPublisher.**

My own IddCx indirect display driver will not load. The UMDF host refuses it at
level 0, before `DriverEntry` runs. Meanwhile **your MttVDD driver loads
perfectly on the same machine, right now** — which is why I'm asking here.

I've eliminated everything I can think of over several days. I'd be grateful for
a pointer, and I suspect the answer is something obvious to anyone who has
shipped one of these.

---

## The failure

```
1003  Driver Manager service is starting a host process for device
      ROOT\DISPLAY\0001
2003  UMDF Host Process has been asked to load drivers for the device
2010  UMDF Host Process has SUCCESSFULLY LOADED drivers for the device
2004  UMDF Host is loading driver iddseat at level 0
2005  UMDF Host loaded C:\Windows\System32\WUDFx02000.dll
2007  The UMDF Host failed to load the driver at level 0.
      The error reported was 3489660941      == 0xD000000D
2900/2901/1006/1008  host shuts down
```

`0xD000000D` is `HRESULT_FROM_NT(STATUS_INVALID_PARAMETER)`.

`setupapi.dev.log` agrees — the package configures cleanly, the devnode is
created, then:

```
!!! dvi: Device not started: Device has problem: 0x1f (CM_PROB_FAILED_ADD),
         problem status: 0xc0000001
```

The driver's own file log is never written, so `DriverEntry` genuinely never
runs. This reproduces on every devnode type I've tried: `devcon install` root
enumeration, `devgen /add`, and `SwDeviceCreate` from a service.

---

## What makes this odd

**MttVDD 25.7.23 loads on this machine.** `ROOT\DISPLAY\0000`, Status OK,
installed with:

```
devcon install MttVDD.inf "Root\MttVDD"
```

It has been running for days and gives me a working virtual display. So the
machine can load IddCx drivers; it just won't load mine.

I also built **Microsoft's WDK IddSampleDriver from source** with my own build
recipe. It loaded **once**, cleanly, and then would not load again on any later
attempt — same binary, same INF, same signing. My own driver has done the same
thing: exactly one successful load, never reproduced.

That "loads once then never again" pattern is the part I find hardest to
explain.

---

## My INF, now a structural clone of MttVDD.inf

I rewrote it to match yours as closely as I could — explicit `AddService`
rather than `Include`/`Needs`, no `Include`/`Needs` in `.NT` or `.NT.hw`,
`DeviceGroupId`, `%12%\UMDF`:

```inf
[Version]
PnpLockdown=1
Signature="$Windows NT$"
ClassGUID = {4D36E968-E325-11CE-BFC1-08002BE10318}
Class = Display
ClassVer = 2.0
Provider=%ManufacturerName%
CatalogFile=iddseat.cat
DriverVer = 08/19/2026,1.2.0.0

[Manufacturer]
%ManufacturerName%=Standard,NTamd64

[Standard.NTamd64]
%DeviceName%=MyDevice_Install, Root\HydraSeat
%DeviceName%=MyDevice_Install, HydraSeat

[SourceDisksFiles]
iddseat.dll=1

[SourceDisksNames]
1 = %DiskName%

[MyDevice_Install.NT]
CopyFiles=UMDriverCopy

[MyDevice_Install.NT.hw]
AddReg = MyDevice_HardwareDeviceSettings

[MyDevice_HardwareDeviceSettings]
HKR,, "UpperFilters",  %REG_MULTI_SZ%, "IndirectKmd"
HKR, "WUDF", "DeviceGroupId", %REG_SZ%, "HydraSeatGroup"

[MyDevice_Install.NT.Services]
AddService=WUDFRd,0x000001fa,WUDFRD_ServiceInstall

[MyDevice_Install.NT.Wdf]
UmdfService=iddseat,iddseat_Install
UmdfServiceOrder=iddseat
UmdfKernelModeClientPolicy = AllowKernelModeClients

[iddseat_Install]
UmdfLibraryVersion=2.33.0
ServiceBinary=%12%\UMDF\iddseat.dll
UmdfExtensions = IddCx0102

[WUDFRD_ServiceInstall]
DisplayName = %WudfRdDisplayName%
ServiceType = 1
StartType = 3
ErrorControl = 1
ServiceBinary = %12%\WUDFRd.sys

[DestinationDirs]
UMDriverCopy=12,UMDF

[UMDriverCopy]
iddseat.dll
```

Still `CM_PROB_FAILED_ADD` after a reboot.

---

## How I build it

I cannot use the WDK's MSBuild integration, so I drive `cl` and `link` directly.
This same recipe compiled Microsoft's `IddSampleDriver` from source and produced
a binary that did load (once).

```
cl /nologo /c /EHsc /std:c++17 /W3 /MT
   /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00
   /DUMDF_USING_NTSTATUS /DWIN32_NO_STATUS
   /DNTDDI_VERSION=0x0A000010
   /I<kit>\Include\10.0.26100.0\um
   /I<kit>\Include\10.0.26100.0\shared
   /I<kit>\Include\10.0.26100.0\km
   /I<kit>\Include\10.0.26100.0\um\iddcx\1.2
   /I<kit>\Include\wdf\umdf\2.33
   iddseat.cpp /Fo:iddseat.obj

link /nologo /DLL /OUT:iddseat.dll iddseat.obj
   <kit>\Lib\10.0.26100.0\um\x64\iddcx\1.2\iddcxstub.lib
   <kit>\Lib\wdf\umdf\x64\2.33\WdfDriverStubUm.lib
   d3d11.lib dxgi.lib avrt.lib advapi32.lib kernel32.lib ole32.lib
   /NODEFAULTLIB:kernel32.lib
```

`dumpbin /exports` shows exactly one export, `FxDriverEntryUm`, which I believe
is correct for UMDF 2.

**Is there a link-time step the MSBuild WDK targets do that I'm missing?** That
is my main suspicion at this point — something the `.props`/`.targets` add that
a hand-rolled `link` line does not.

---

## Things I have already ruled out

Each of these was tested and made no difference:

- **UMDF version** — 2.35, 2.33, 2.25, 2.15. `WUDFx02000.dll` is the UMDF 2.x
  host for all minor versions, so this was never the variable.
- **IddCx version** — built against 1.11, 1.10 and 1.2 stubs.
- **`UmdfExtensions`** — absent, `IddCx0110`, and `IddCx0102`. The registry says
  `IddCx0102` is the only registered extension:
  `HKLM\SYSTEM\CurrentControlSet\Control\Wdf\UMDF\IddCx\Versions\1\2` →
  `Service = IddCx0102`.
- **`IndirectKmd` upper filter** — added; confirmed present on the devnode.
- **`ClassVer = 2.0`** — added.
- **`ServiceBinary`** — both `%13%\iddseat.dll` and `%12%\UMDF\iddseat.dll`.
- **Signing** — `testsigning Yes`, cert in `LocalMachine\Root` **and**
  `TrustedPublisher`, `Inf2Cat` clean, catalog Valid.
- **Correct binary staged** — DriverStore hash matches my build.
- **`UmdfHostProcessSharing`, `UmdfKernelModeClientPolicy`,
  `UmdfFileObjectPolicy`, `UmdfFsContextUsePolicy`** — every combination.
- **`SwDeviceCreate` parameters** — with and without device properties, with and
  without compatible IDs, `Root\`-prefixed and bare hardware IDs.
- **Stale class instance keys and orphaned devnodes** — audited and removed.
- **`HKR,, Security, , "D:P(A;;GA;;;WD)"`** — taken from Microsoft's
  `rdpidd.inf`. This produced my **only ever successful load**, but it did not
  reproduce, and MttVDD works with no descriptor at all — so I don't think it is
  the answer.

---

## What I would like to know

1. **Is there a build step I'm missing** by driving `cl`/`link` by hand instead
   of using the WDK's MSBuild targets? Something injected at link time, or a
   resource/section the loader checks?

2. **Does `0xD000000D` at level 0 have a known meaning** beyond "invalid
   parameter"? Is there a way to get the framework to say *which* parameter?
   I enabled WUDF WPP tracing (`LogEnable=1`, `LogLevel=15`) and the ETW session
   runs and reports buffers written, but `C:\ProgramData\Microsoft\WDF\WUDFTrace.etl`
   is never created.

3. **Has anyone made MttVDD a remote-session driver** —
   `IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER`, instantiated by the RD stack
   rather than root-enumerated? That is my actual goal: a virtual display inside
   a second Windows session, so two people can use one machine. I'd happily use
   MttVDD unmodified if the remote-session path is known to work.

Happy to provide the full source, the complete WUDF event sequence, or
`setupapi.dev.log` extracts. Thanks for reading — and for shipping a driver that
works, which has been the most useful reference I have.
