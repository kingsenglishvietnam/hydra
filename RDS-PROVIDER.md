# RDS Protocol Provider — investigation, 2026-08-11

The architecturally correct version of Hydra. Windows has a supported API for
exactly what this project does: attach your own input and output devices to a
Windows session, supplying everything else yourself. It does not use the RDP
protocol at all.

Status: **mechanism fully mapped, sample builds, nothing written yet.**

---

## Why this matters

If Hydra were a registered protocol provider:

| Today | With a provider |
|---|---|
| RDP-Wrapper creates the session | the RD service creates it, on your call |
| `hydrardp` / `sdl-freerdp` holds it open | no client at all |
| DDA capture or FreeRDP decode | the IDD **is** the display; frames arrive because your driver is the monitor |
| Interception + loopback TCP + `seatB_agent` injects | `GetInputHandles` hands you keyboard/mouse/beep |
| a crashing client wedges the stack (PROBLEM 5) | no RDP stack in the path |
| client window visibility gates composition (PROBLEM 1) | no client window |

PROBLEM 2 is already fixed, but would become moot.

---

## What was established today

**Remote-session IDDs are a supported category.** An IDD declares itself one by
setting `IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER` in `IDDCX_ADAPTER_CAPS.Flags`
at `IddCxAdapterInitAsync`. The OS fails the call if the flag does not match how
the device was actually created — so it cannot be spoofed, and does not need to
be: when the RD stack instantiates the driver, the flag is simply true.

**The junction is `IWRdsWddmIddProps`**, implemented on the *connection* object:

- `GetHardwareId(WCHAR*, DWORD)` — returns the hardware ID from your IDD's INF.
  The RD stack then loads that driver **into the session**.
- `OnDriverLoad(SessionId, DriverHandle)` — handle back to talk to it
- `OnDriverUnload(SessionId)`
- `EnableWddmIdd(BOOL)` — TRUE selects IDD over legacy XDDM

Because the driver is loaded into the seat's session, the cross-session D3D11
wall that killed the original `iddseat` topology never arises. That wall is
documented in `common/hydra_ipc.h`: `OpenSharedResourceByName` fails the
handover with `E_INVALIDARG` across a TS session boundary regardless of flags.

**`GetVideoHandle` is documented "not required if using IDD."** The display is
not a handle you feed — it is the driver. No capture, no duplication.

---

## How a connection is made (`TestProtocolAPI.cpp`, ~15 lines)

No socket, no handshake, no protocol:

1. `CComObject<CWRdsProtocolConnection>::CreateInstance`
2. `SetCredentials(domain, user, password)`
3. `ZeroMemory` the `WRDS_CONNECTION_SETTINGS`, then set
   `WRdsConnectionSettingLevel = LEVEL_1` and
   `WRdsListenerSettingLevel = LEVEL_1` — **connections fail if these are unset**
4. `pListenerCallback->OnConnected(pConnection, &settings, &connCallback)`
5. `connCallback->GetConnectionId(...)`, `pConnection->SetConnectionCallback(...)`
6. `connCallback->OnReady()`

Windows then creates the session. The sample's trigger is polling for
`C:\TestProtocol\createconnection.txt` every 5 seconds; for Hydra it would be
`hydrad` saying go.

`StartListen` spawns a thread and holds the callback — must `AddRef`, and
`Release` in `StopListen`.

---

## Start sequence (from the docs)

1. service reads the listener name and manager CLSID from the registry
2. `Initialize` on the manager
3. `CreateListener` — once per registered listener; only ONE manager object is
   created regardless of how many listeners
4. `StartListen` on each listener
5. …running…
6. `StopListen`, then `Uninitialize`

The listener creates the connection object when a client connects and calls
`OnConnected`; the service returns an `IWRdsProtocolConnectionCallback` which
the protocol must release when the connection closes.

---

## Registration — one registry value

Create a key per listener as a sibling of `RDP-Tcp`:

```
HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\<ListenerName>
```

with `REG_SZ` value **`LoadableProtocol_Object`** = the manager's CLSID.

The sample's CLSID, from `WRdsProtocolManager.rgs`:
`{23b3ed19-0299-45bd-b235-0c0c9bab40a4}`

Microsoft's own RDP manager for comparison, under `RDP-Tcp`:
`{5828227c-20cf-4408-b73f-73ab70b8849f}` — same slot, no special casing.

Other reference values from `RDP-Tcp` worth carrying over:
`fEnableWinStation = 1` (or the listener is ignored),
`MaxInstanceCount = 4294967295`, `PortNumber`, `WdName`, `WdPrefix`.

Only two listeners exist on this machine: `Console` and `RDP-Tcp`. RDP-Wrapper
does **not** register a listener — it patches policy. Ours would be the third.

The `.rgs` files only register the COM classes under `HKCR\CLSID` with
`ThreadingModel = Free`. `regsvr32` does that half; the WinStations key is
separate and unscripted.

---

## Build

Clone at `C:\Programs\rdsprov`. Builds under **both** toolsets — the difference
is which ATL library variants are installed:

VS 2026 (`\Microsoft Visual Studio\18\Community`) — only *spectre* ATL libs, so
the mitigation flag is required:

```powershell
msbuild TestProtocol_Ext.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.28000.0 /p:SpectreMitigation=Spectre /v:minimal
```

VS 2022 BuildTools — only *plain* ATL libs (after adding the ATL component), so
the flag must be absent:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' TestProtocol_Ext.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.28000.0 /v:minimal
```

Output: `Sample\x64\Release\TestProtocol_Ext.dll`

`wtsprotocol.h` / `.idl` are present in SDK 10.0.28000.0 — the same version
`build-driver.ps1` pins.

---

## THE NEXT STEP, and it is the dangerous one

Registering the provider makes `termsrv` load the DLL into its svchost at
service start. **A bad provider takes Terminal Services down**, which stops
seat B and possibly RDP entirely.

Before registering anything:

- do it on a morning with no lessons
- know how to undo it without RDP: the recovery is deleting the WinStations key,
  so have local console access and the exact key path written down
- `reg export` the WinStations subtree first
- consider a restore point

Then, in order:

1. `regsvr32 TestProtocol_Ext.dll` (registers the COM classes only)
2. create `WinStations\TestProtocol` with `LoadableProtocol_Object` =
   `{23b3ed19-0299-45bd-b235-0c0c9bab40a4}` and `fEnableWinStation = 1`
3. restart `TermService` — watch whether it comes back
4. if it does, `mkdir C:\TestProtocol` and touch `createconnection.txt` to
   trigger the sample's connection path
5. `query session` — a session named `TL-Ext` would mean it worked

Step 3 is the real test. Everything before it is reversible in seconds.

---

## Still unknown

**Whether the SKU session cap applies to a custom provider's listener.**
RDP-Wrapper patches `SingleUserOffset`, `DefPolicyOffset`, `SLPolicyInternal`
and `SLPolicyOffset` inside `termsrv.dll` — and `termsrv` IS the RCM that loads
providers, so the patched policy should sit below the provider layer and apply
equally. That is inference, not fact. `SessionArbitrationEnumeration` takes a
`bSingleSessionPerUserEnabled` argument and `E_NOTIMPL` yields default
arbitration, so the provider at least participates in that decision.

**`GetInputHandles` has no sample implementation** — it is a TODO in the
Microsoft sample. Keyboard, mouse and beep handles. This is the one place with
no reference code, and it is the piece that would replace Interception,
`seat_router` and `seatB_agent`.

**Reworking `iddseat` as a remote adapter.** It exists and built once
(`build-driver.ps1`, WDK 28000 / IddCx 1.11 / UMDF 2.35) but was written as a
console IDD. Needs the remote flag, and signing.

---

## Honest scale

The provider itself is small — the whole Microsoft sample is ~450 lines of which
maybe 40 do anything, and it is less code than the FreeRDP client already
written. The weight is in `GetInputHandles`, the driver rework, and signing.

Not a weekend. But every piece is documented, the sample builds, and the parts
Hydra already has — the pixel ring, `mirror`, the input wire format — are the
parts that would be reused.
