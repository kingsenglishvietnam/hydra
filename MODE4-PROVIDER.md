# MODE4-PROVIDER.md — the last piece

Written 2026-08-20, after the remote IDD started working.

**The driver is done.** What remains is the provider holding its session open.

---

## Where things stand

`iddseat` remote build, on an RD-stack devnode, every cycle:

```
DriverEntry
DeviceAdd: entered
DeviceAdd: seat properties read
IddCxDeviceInitConfig    -> 0x00000000
WdfDeviceCreate          -> 0x00000000
IddCxDeviceInitialize    -> 0x00000000
D0Entry: caps.Flags=0x5              <-- REMOTE_SESSION_DRIVER | USE_SMALLEST_MODE
IddCxAdapterInitAsync    -> 0x00000000
AdapterInitFinished      -> 0x00000000
IddCxMonitorCreate       -> 0x00000000
ParseMonitorDescription: 1920x1080@60
IddCxMonitorArrival      -> 0x00000000
```

Clean, three times running. **Remote-session IDDs are reachable on 24H2 build
26100** — that question is settled, and the answer is yes, with the IddCx 1.10
stub while the INF declares `IddCx0102`.

And the provider, same runs:

```
EnableWddmIdd(1)
AcceptConnection
GetClientData
AuthenticateClientToSession
NotifySessionId
GetInputHandles
GetHardwareId                        count=200
ConnectNotify session=2
NotifyCommandProcessCreated session=2
PreDisconnect reason=18              <-- ~0.4s later
OnDriverUnload session=2
DisconnectNotify
Close
```

The session is created, authenticated, the shell process is launched, the
display arrives — and then termsrv disconnects it.

---

## What reason=18 is not

`CWRdsProtocolConnection::PreDisconnect` logs and returns `S_OK`. It does not
initiate anything. **termsrv is deciding to disconnect**, and the provider is
only being told.

The reason code has moved as the driver improved, which is itself informative:

| reason | when | meaning |
|---|---|---|
| 17 | ~1.2s, broken driver staged | the IDD failed to load |
| 12 | ~33s, no IDD at all | timed out waiting |
| **18** | ~0.4s, IDD working | **new — something after the display arrives** |

So 18 is not a display problem. Everything up to and including
`IddCxMonitorArrival` succeeds.

---

## The likely cause

`TestProtocol_Ext` is Microsoft's **sample** protocol provider. It creates a
session and implements the connection interface, but it has no real input or
output channels — no wire protocol, which is the whole point of using it.

termsrv very likely concludes the client is gone and tears the session down.
A real protocol provider would be servicing something continuously; this one
returns `S_OK` and waits.

The methods that would keep a session alive are the ones to look at.

---

## Step 1 — find what termsrv expects and is not getting

Read the whole connection class, not fragments:

```powershell
Get-Content C:\Programs\rdsprov\Sample\TestProtocol_Ext\WRdsProtocolConnection.cpp
```

What to look for, in rough order of likelihood:

- **`GetLogonErrorRedirector` / `GetInputHandles`** — the input handles are
  returned once. If they are invalid or closed, termsrv would drop the session
  as soon as it tried to use them. `GetInputHandles` is logged and returns, but
  nothing checks whether the handles it hands back are live.
- **`NotifyCommandProcessCreated`** — the last thing that succeeds. Whatever
  termsrv does next is what fails.
- **`GetProtocolStatus` / `GetLastInputTime`** — if termsrv polls these and the
  sample returns zero or an error, an idle-timeout disconnect follows.
- **`ConnectNotify` returning before the session is ready** — a race rather than
  a missing method.

The sample ships with commented-out sections and `// Optional:` markers
throughout. One of those optional methods is probably not optional for a session
that has to persist.

## Step 2 — instrument every remaining method

The pattern that solved the driver: log every call and every return value, then
read rather than theorise.

`HydraProvLog` already exists. Add it to every method in
`WRdsProtocolConnection.cpp` that does not yet have it, including the ones that
just `return S_OK`. Then trigger and read the sequence.

**What you want to see:** which method termsrv calls last before `PreDisconnect`.
That is the one to fix, and right now several methods are silent so the gap is
invisible.

## Step 3 — the alternative, if the sample cannot hold a session

`rdpidd.inf` and `RdpIdd_IndirectDisplay` are Microsoft's own remote IDD, and
they work on this machine. There are five `RdpIdd_IndirectDisplay&SessionId_*`
devnodes in the Enum right now.

So an **ordinary RDP session already has a working remote IDD**. If
`session_capture` can duplicate that display directly rather than reading the
client's framebuffer, you get most of mode 4's benefit with no provider at all —
and modes 1, 2, 3, 6 and 7 already create such sessions.

That is `TIER1-PLAN.md` step 1, still unexplored, and it may be less work than
finishing the provider.

---

## Before any of it

**Run `safety-gate.ps1` before every driver install.** It was skipped on 08-20
and the machine came up with a blank screen — recovered by unplugging the USB
hub and rebooting, but that was luck rather than process.

```powershell
.\safety-gate.ps1 -Label "<what you are about to do>"
```

**The provider must be unregistered when not testing.** A registered listener
plus a leftover `C:\TestProtocol\createconnection.txt` makes the next boot fire
a connection unprompted — which is how a `HydraSeat_RemoteIDD_v1&SessionId_0002`
devnode appeared on its own after a reboot.

**Restore `rdpwrap.dll` after every provider session**, or modes 1–3, 6 and 7
lose their second session:

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\Program Files\RDP Wrapper\rdpwrap.dll' -Type ExpandString
Restart-Service TermService -Force
$p=(Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId; (Get-Process -Id $p -Module).ModuleName | Where-Object { $_ -match 'rdpwrap|termsrv' }
```

Both names must appear.

**Phantom Enum devnodes need SYSTEM to remove.** `Remove-Item` reports success
and does nothing:

```powershell
& C:\Programs\PSTools\PsExec64.exe -s -accepteula reg delete "HKLM\SYSTEM\CurrentControlSet\Enum\SWD\REMOTEDISPLAYENUM\<key>" /f
```

They do not appear to block new devnodes, so this is housekeeping rather than a
prerequisite.

**Keep the driver store clean.** Eleven stale `iddseat` packages accumulated in
one afternoon. Two is correct — one console, one remote:

```powershell
$e = (pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n"; for ($i=0; $i -lt $e.Count; $i++) { if ($e[$i] -match 'Original Name:\s+iddseat') { "{0} <- {1}" -f ($e[$i-1] -replace '.*:\s+',''), ($e[$i] -replace '.*:\s+','') } }
```

---

## The method that worked, worth repeating

Three bugs, five days, and every one was found by **reading a working
reference** — the WDK IddCx sample and `MttVDD.inf` — rather than reasoning
about what might be wrong.

1. `UMDF_VERSION_MAJOR` / `UMDF_VERSION_MINOR` missing from the compile line.
   The WDK MSBuild targets supply them; a hand-rolled `cl` does not.
   `WDF_BIND_INFO` is built from them and handed to the framework by
   `FxDriverEntryUm` **before** `DriverEntry`, so the bind was malformed and the
   load was refused with `0xD000000D`.
2. `EndPointDiagnostics.pFirmwareVersion` / `pHardwareVersion` left null. The
   sample marks them `(required)`. `IddCxAdapterInitAsync` returns
   `STATUS_INVALID_PARAMETER` without them.
3. `FillSignal` added a blanking interval, so `totalSize != activeSize`. An
   indirect display has no real signal and no blanking; the sample sets them
   equal. `IddCxMonitorArrival` rejects a mode with blanking.

Every hypothesis generated by reasoning failed. Every fix came from a file
already on disk.

The second thing that mattered: **instrumentation before theory.** The
breakthrough came from `OutputDebugStringA` alongside the file log — `WUDFHost`
runs as LOCAL SERVICE and could not write `C:\Windows\Temp`, and that missing
file had been read as "DriverEntry never runs" for five days. Once every call
logged its return value, each bug took minutes.

Do that first with the provider.
