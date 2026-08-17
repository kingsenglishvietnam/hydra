# MODE4-STEP2.md — remote-session IDD from Microsoft's sample

Written 2026-08-17. Follows MODE4-PLAN.md step 1, which is now **passed by a
different route than planned**: VDD's source will not build into a loadable
driver here, but **Microsoft's IddSampleDriver, compiled from source with
`build-mssample.ps1`, DID load** — events 2003 → 2010 → 2004, no 2007, on
`SWD\IDDSAMPLEDRIVER\IDDSAMPLEDRIVER`.

So the base is Microsoft's sample, not VDD.

**Why VDD's source fails and its shipped binary works:** its code calls
`IddCxAdapterSetRenderAdapter` (IddCx 1.4+) guarded by
`IDD_IS_FUNCTION_AVAILABLE`, while its INF declares `IddCx0102`. Building that
against 1.10 headers produces a driver whose declared contract and compiled
contract disagree. Their shipped build reconciles it with WDK MSBuild settings
we are not reproducing. Not worth chasing — the sample is simpler and already
works.

---

## 0. Before anything: restore mode 6

Mode 6 is a working teaching seat and must not be left broken.

```powershell
$d='C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe'
Get-PnpDevice | Where-Object FriendlyName -match 'Virtual Display' | ForEach-Object { & $d /remove $_.InstanceId }
(pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n" | Select-String 'mttvdd' -Context 1,0
```

Delete the from-source package (whatever `oemNN.inf` that shows), then:

```powershell
pnputil /add-driver C:\Programs\vdd\VirtualDisplayDriver\MttVDD.inf /install
& $d /add /hardwareid "Root\MttVDD"
Start-Sleep 8
Get-PnpDevice | Where-Object FriendlyName -match 'Virtual Display' | Select-Object FriendlyName, Status
```

**GATE: Status OK.** If a reboot is demanded, take it — `devgen /remove` leaves
pending operations and every result read before that reboot is unreliable. This
cost two false readings today.

Set the virtual display back to 1920x1080 at `(0,-1080)` in Display settings if
the removals reset it.

---

## 1. Confirm the sample still loads

Do not skip. Everything below assumes a known-good starting point, and the
package set has churned a lot.

```powershell
cd C:\Programs\hydra
.\build-mssample.ps1 -Install
```

Leave `build-mssample.ps1` at its original settings — `/MT`, IddCx `1.2`, no
`ucrt.lib`. That is the combination that loaded. Do not apply the CRT changes
made to `build-vdd.ps1`; they were chasing VDD's problem, not this one.

```powershell
pnputil /add-driver C:\Programs\hydra\dist\mssample\IddSampleDriver.inf /install
Start-Process C:\Programs\wdksample\video\IndirectDisplay\x64\Release\IddSampleApp.exe
Start-Sleep 10
Get-PnpDevice | Where-Object InstanceId -match 'IDDSAMPLE' | Select-Object Status, Problem
Get-WinEvent -LogName 'Microsoft-Windows-DriverFrameworks-UserMode/Operational' -MaxEvents 6 | Select-Object TimeCreated, Id
```

**GATE: Status OK, no event 2007.** If this fails, the sample INF was edited
during today's work — `build-mssample.ps1` copies the pristine one from
`C:\Programs\wdksample`, so re-running it restores the original. Verify:

```powershell
Select-String -Path C:\Programs\hydra\dist\mssample\IddSampleDriver.inf -Pattern 'MyDevice_Install,'
```

Must read `Root\IddSampleDriver` and `IddSampleDriver`, not `HydraSeat`.

Kill the app afterwards — its device is a phantom once the process exits, which
is normal and not a failure:

```powershell
Get-Process IddSampleApp -EA SilentlyContinue | Stop-Process -Force
```

---

## 2. Make a Hydra copy of the sample source

Do not edit the sample tree in place. It is the reference and must stay pristine
for comparison.

```powershell
New-Item -ItemType Directory -Force C:\Programs\hydra\iddremote | Out-Null
Copy-Item 'C:\Programs\wdksample\video\IndirectDisplay\IddSampleDriver\Driver.cpp' C:\Programs\hydra\iddremote\ -Force
Copy-Item 'C:\Programs\wdksample\video\IndirectDisplay\IddSampleDriver\Driver.h'   C:\Programs\hydra\iddremote\ -Force
Copy-Item 'C:\Programs\wdksample\video\IndirectDisplay\IddSampleDriver\IddSampleDriver.inf' C:\Programs\hydra\iddremote\iddremote.inf -Force
Get-ChildItem C:\Programs\hydra\iddremote | Select-Object Name, Length
```

---

## 3. The two source changes

Find the adapter caps:

```powershell
Select-String -Path C:\Programs\hydra\iddremote\Driver.cpp -Pattern 'IDDCX_ADAPTER_CAPS|AdapterCaps|IddCxAdapterInitAsync' -Context 4,14
```

Set the flags. `iddseat.cpp` does the same thing under `/DHYDRA_REMOTE_IDD`:

```c
AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER
                  | IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;
```

`USE_SMALLEST_MODE` is **mandatory** alongside the remote flag.

**The hard gate to remember:** the OS fails `IddCxAdapterInitAsync` when the
remote flag is set on a device the RD stack did not create, *and* when it is
absent on one the stack did. **A remote IDD cannot be tested with `devgen` or
the sample app.** It only comes up through `HydraProto`. So from here, every
test needs the provider.

### The version problem, and how to handle it

`IddCxAdapterDisplayConfigUpdate2` is documented as required for a remote
session's display paths to activate (`3465b75`), and it needs **IddCx 1.10
minimum**. The only registered class extension on this machine is
`IddCx0102` — `HKLM\SYSTEM\CurrentControlSet\Control\Wdf\UMDF\IddCx\Versions\1\2`
has `Service = IddCx0102`.

**Try without it first.** Set the two flags, build against 1.2, declare
`IddCx0102`, and see whether the session gets a display. The call may only be
needed for multi-monitor or hot-resize cases.

If paths never activate, the options in order of cost:

1. Build against 1.10 headers with `IDDCX_VERSION_MAJOR=1`,
   `IDDCX_VERSION_MINOR=10`, `IDDCX_MINIMUM_VERSION_REQUIRED=3` while keeping
   `UmdfExtensions = IddCx0102`, and guard the call with
   `IDD_IS_FUNCTION_AVAILABLE` — the pattern VDD uses.
2. If that will not load, **remote-session IDDs may not be reachable on this
   build of 24H2.** That is a real possible outcome and worth accepting rather
   than grinding. Mode 6 already gives a working seat.

---

## 4. The remote INF

`iddseat-remote.inf` documents this correctly even though its driver never
loaded. Copy the shape, not the file.

```powershell
Select-String -Path C:\Programs\hydra\iddseat\iddseat-remote.inf -Pattern 'HardwareId|Umdf|AddReg|HKR|CatalogFile|ServiceBinary|DestinationDirs|UMDriverCopy' | Select-Object LineNumber, Line
```

Required in `iddremote.inf`:

- **Bare hardware ID, no `Root\` prefix** — the RD stack creates the devnode.
  Use something distinct, e.g. `HydraSeat_RemoteIDD_v1`.
- `UmdfHostProcessSharing = ProcessSharingDisabled` — one host per session.
- **No `DeviceGroupId`** — grouping defeats per-session isolation. The sample
  sets one; remove it.
- `UmdfExtensions = IddCx0102`
- `UmdfLibraryVersion = 2.33.0`
- `ServiceBinary = %12%\UMDF\iddremote.dll`, `DestinationDirs UMDriverCopy = 12,UMDF`
- `AddReg` in `.NT.hw` with `HKR,, "UpperFilters", 0x00010000, "IndirectKmd"`
- **Its own `CatalogFile` and service name**, so it can sit in the store
  alongside the console sample without colliding.

---

## 5. Build it

Copy `build-mssample.ps1` and point it at the new source:

```powershell
Copy-Item C:\Programs\hydra\build-mssample.ps1 C:\Programs\hydra\build-iddremote.ps1 -Force
```

Then change `$Sample` to `C:\Programs\hydra\iddremote`, the INF name to
`iddremote.inf`, the DLL name to `iddremote.dll`, and `$OutDir` to
`C:\Programs\hydra\dist\iddremote`.

**Keep `/MT`, IddCx `1.2`, and no `ucrt.lib`.** That is the loading combination.

The WPP stub and the empty `Driver.tmh` are still needed — the sample uses
`WPP_INIT_TRACING`, and without the WDK MSBuild pass nothing generates the
`.tmh`. `build-mssample.ps1` already handles both.

```powershell
.\build-iddremote.ps1 -Install
```

---

## 6. Provider

The provider does **not** need RDP-Wrapper — proven 2026-08-14, console session
and `hydraproto#0` concurrent under stock `termsrv`. But RDP-Wrapper must be out
of the way or `TestProtocol_Ext.dll` never loads at all.

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\WINDOWS\System32\termsrv.dll' -Type ExpandString
Restart-Service TermService -Force; Start-Sleep 10
```

Stage the driver **before** registering the provider, so the RD stack finds it
when it asks:

```powershell
.\safety-gate.ps1 -Label "iddremote"
pnputil /add-driver C:\Programs\hydra\dist\iddremote\iddremote.inf /install
```

```powershell
cd C:\Programs\hydra; .\rdsprov-register.ps1 -Register -Apply
```

**`-Register` recreates the listener key EMPTY every time.** Six occurrences so
far. `Domain` must be **empty**, not the machine name:

```powershell
$k='HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto'; $s=Read-Host 'password for teacher' -AsSecureString; $b=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($s); Set-ItemProperty $k -Name Username -Value 'teacher'; Set-ItemProperty $k -Name Domain -Value ''; Set-ItemProperty $k -Name Password -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)); [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b); Get-ItemProperty $k | Select-Object Username, Domain, @{n='PwLen';e={$_.Password.Length}}
```

---

## 7. Trigger and read

```powershell
cd C:\Programs\hydra; Remove-Item C:\ProgramData\Hydra\provider.log, C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; Restart-Service TermService -Force; Start-Sleep 10; New-Item -ItemType File -Force C:\TestProtocol\createconnection.txt | Out-Null; Start-Sleep 30; query session; Get-Content C:\ProgramData\Hydra\provider.log
```

### Reading `provider.log`

```
EnableWddmIdd(1)          <-- termsrv TELLING us the mode, not asking
AcceptConnection
GetClientData
AuthenticateClientToSession
NotifySessionId
GetInputHandles
GetHardwareId             *** the stack asking for the driver *** count=200
ConnectNotify session=N   (IDD creation starts here)
NotifyCommandProcessCreated session=N
```

| what you see | meaning |
|---|---|
| stops at `NotifyCommandProcessCreated`, session persists | **the win** — go to §8 |
| `PreDisconnect reason=17` at ~1.2s | driver failed to load. Check event 2007. |
| `PreDisconnect reason=12` at ~33s | timed out. With no IDD this is the known stall. |
| no `AcceptConnection` at all | listener did not fire — credentials, or a stale trigger file |

```powershell
Get-WinEvent -LogName 'Microsoft-Windows-DriverFrameworks-UserMode/Operational' -MaxEvents 8 | Select-Object TimeCreated, Id, Message | Format-List
```

Check timestamps are **fresh**. Reading stale events cost three false
conclusions today.

---

## 8. Verify the session actually got a desktop

This is the thing that has never happened. With no IDD, a provider session
authenticates (Security 4624, Logon Type 10, no 4625) and then stalls at
`LogonUI` forever — Winlogon 7001 never fires, warm profile or cold.

```powershell
$id = (query session | Select-String 'hydraproto#').ToString().Trim() -split '\s+' | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1
Get-CimInstance Win32_Process -Filter "SessionId=$id" | Select-Object Name | Sort-Object Name
```

**`explorer.exe` and `dwm.exe` present, `LogonUI.exe` ABSENT** means logon
completed — which only happens if the session has a display.

Then point capture at it and read the ring:

```powershell
Select-String -Path C:\Programs\hydra\seats.toml -Pattern '^session|^display_mode'
```

`display_mode = "capture"`, `session` matching that user, then:

```powershell
.\setup.ps1; Start-Service Hydra; Start-Sleep 8; .\dist\hydractl.exe status
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

`seq` and `frame` advancing = **mode 4 is real**, and the RDP client leaves the
pixel path entirely.

---

## 9. Cleanup — every time, without exception

Leaving the listener registered with the trigger file present makes the next
boot fire a connection on its own.

```powershell
cd C:\Programs\hydra; Stop-Service Hydra -EA SilentlyContinue; Remove-Item C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; .\rdsprov-register.ps1 -Unregister -Apply
```

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\Program Files\RDP Wrapper\rdpwrap.dll' -Type ExpandString
Restart-Service TermService -Force; Start-Sleep 5
$p=(Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId; (Get-Process -Id $p -Module).ModuleName | Where-Object { $_ -match 'rdpwrap|termsrv' }
```

**Both names must be listed** or modes 1–3 silently lose their second session.

Emergency undo if the provider takes RDP down — physical console only:

```
reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
sc stop TermService
sc start TermService
```

---

## 10. Rules earned today

- **Reboot when Windows says a reboot is required.** `devgen /remove` and
  package swaps leave `CM_PROB_NEED_RESTART`, and results read before the reboot
  are meaningless. Two false conclusions today.
- **Never stage two packages claiming the same hardware IDs.** It broke a driver
  that had been loading fine an hour earlier.
- **Check event timestamps before drawing conclusions.** Three false readings.
- **Verify a patch landed before building.** One edit was silently lost to a
  file restore and produced twenty minutes of debugging a binary that did not
  contain it.
- **`safety-gate.ps1` before every `pnputil /add-driver`.**
- **Filter `pnputil` output precisely.** A loose `oem22[0-9]` pattern got two
  unrelated system drivers deleted.
