# HANDOFF.md — everything needed to start from zero

Assembled 2026-08-13 by walking back through every session on this project. If
you are a new assistant, a new machine, or Nathan in six months, **read this
before touching anything.**

Companion files, in reading order:
`MODES.md` (how to run it) → `REBUILD.md` (how to rebuild the machine) →
`INCIDENT-2026-08-12.md` (what went wrong) → `OPEN-PROBLEMS.md`.

---

## 1. Who and where

Nathan, ELT teacher, Ho Chi Minh City, running King's English Program (kings.vn).
Hydra is a personal engineering project that turns his one teaching machine into
two isolated workstations, so a student can work at a second seat during lessons.
The second seat's Windows account is literally named `teacher`; the console
account is `user`.

**Working style, learned the hard way:**

- Terse. Wants the command, not the essay.
- **Clipboard reversal**: multi-line paste can arrive reversed or fused. Give
  single-line commands, or semicolon-joined one-liners, or write a `.ps1`.
- Attachments sometimes arrive empty.
- **Read the source before theorising.** This is the single most repeated
  correction. The gfx crash took eleven failed hypotheses and one source read.
  Error 87 sat behind an OS reinstall and was findable by grep.
- Verify every step. Don't assert paths, versions, or behaviour without checking.
- Dry humour welcome. Being wrong confidently is not.

---

## 2. Hardware and environment

| | |
|---|---|
| machine | Surface Book 3, Windows 11 24H2, build 26100 |
| GPU | Intel iGPU + NVIDIA GTX 1660 Ti Max-Q (muxless, render-only, **no display outputs**) |
| repo | `C:\Programs\hydra` → `github.com/kingsenglishvietnam/hydra` (private) |
| logs | `C:\ProgramData\Hydra\logs\` |
| toolchain | VS Build Tools 17.14.37, cl 19.44 x64; WDK 26100 + 28000; MSYS2/MinGW64 |
| shell | PowerShell 7 (`pwsh`). All commands in PS syntax. |

The muxless GPU is why VFIO/hypervisor multiseat was impossible and why this is
an RDP-loopback design at all.

**Tags:** `gfx-working`, `rdsprov-session`, `idd-builds`, `rdsprov-idd-staged`,
`post-reset-2026-08-13`.

---

## 3. What Hydra actually is

One Windows box, two seats. Seat A is the console (`user`, Surface panel). Seat B
is an RDP-Wrapper session (`teacher`, external 1920x1080 monitor) with its own
keyboard, mouse and audio.

```
                    ┌─ RDP-Wrapper: 2 concurrent sessions
                    │
console session 1   │   seat B session (teacher)
  seat_router ──────┼──▶ seatB_agent          (input, TCP 56789)
  (Interception)    │      └─ SendInput
                    │
  mirror.exe ◀──────┼─── session_capture      (pixels, shared memory)
  (draws panel)     │      └─ DDA + cursor composite
                    │
  abren:B ◀─────────┼─── abcap:B              (audio, shared memory)
  (monitor endpoint)│      └─ WASAPI loopback
                    │
  hydrad.exe (service, session 0) supervises all of the above
  hydractl.exe (control) ── \\.\pipe\hydra_control
```

### The architectural fact that shapes everything

**D3D11 shared textures do not cross a Terminal Services session boundary.**
`OpenSharedResourceByName` resolves the name then fails the handover with
`E_INVALIDARG`, regardless of access flags.

This killed the original design (IDD in one session, consumer in another) and is
why capture runs *inside* seat B's own session and frames travel as **CPU pixels
through a seqlock-protected shared-memory ring**. Documented in
`common/hydra_ipc.h`. Do not propose D3D11 sharing again.

Corollary worth remembering: the pixel ring itself crosses sessions fine — it is
plain shared memory. An IDD could write into it with a readback, which the header
says is affordable at 1080p.

### Shared memory (`common/hydra_ipc.h`)

| section | struct | carries |
|---|---|---|
| `Global\HydraSeat_<seat>_meta` | `HydraSeatMeta` | `ready`, `frame`, **`stalled`** |
| `Global\HydraSeat_<seat>_pix` | `HydraSeatPixels` | `seq`, dims, pitch, `curX/curY/curSeq` |
| `Global\HydraSeat_<seat>_aud` | — | `writePos`, rate, channels, `running` |

**`stalled` is the diagnostic nobody reads.** Non-zero means the producer is
attached to the seat's desktop but `EnumOutputs` returns no duplicatable display
— a permanent failure that looks *identical* to healthy-and-idle from outside:
process alive, log quiet, `hydractl` reporting "running". It is PROBLEM 1's first
candidate and it was built, published, and then ignored in favour of the two
signals its own comment warns are useless.

**`seq` is the real liveness signal**, not mirror's CPU. It increments per
published frame. Flat `seq` = stopped pipeline, definitively.

Read it with `.\hydra-shm.ps1` (elevated — `Global\` namespace).

### Input wire format

9 bytes, packed little-endian: `kind u8, a u16, b u16, dx i16, dy i16`. Must
byte-match across `seat_router.c`, `seatB_agent.c` and the legacy
`seatB_agent.py` prototype.

Absolute-pointer move modes are encoded in spare high bits of `a`
(`0x1000`/`0x2000`, above the `0x800` state ceiling); u16 coordinates are
bit-preserved through the i16 fields.

---

## 4. Components

| binary | session | job |
|---|---|---|
| `hydrad.exe` | 0 (service `Hydra`) | supervises everything; absorbed `respawn.exe`'s backoff contract |
| `hydractl.exe` | any | `status \| reload \| restart <seat\|all> \| display <seat> <idd\|capture> \| audiofix <seat> \| learn \| stop` |
| `seat_router.exe` | 1 (console) | Interception capture, hardware-ID match, forward over TCP |
| `seatB_agent.exe` | seat, **as SYSTEM** | replay via `SendInput`; publishes `GetCursorPos` into `curX/curY/curSeq` |
| `session_capture.exe` | seat, as user | DDA + cursor composite → pixel ring |
| `mirror.exe` | 1 | draws the ring. `mirror B \\.\DISPLAY2` = seat form; `--window` = debug |
| `hydrardp.exe` | 1 | headless FreeRDP client (mode 3) |
| `audio_bridge.exe` | seat + console | `abcap` captures, `abren` renders |
| `clip_console.exe` | 1 | `WH_MOUSE_LL` cursor clamp (retired approach — see MODES.md) |
| `route_endpoint.exe` | 0 | `--list` enumerates audio endpoints |
| `audiotest.exe` | 1 | cross-session loopback verifier |
| `offs.exe` | — | prints `offsetof` values, for resolving crash addresses |

**Why `seatB_agent` runs as SYSTEM** (`hydrad.cpp` ~369): a medium-integrity user
token cannot attach to or inject into a desktop the user doesn't own, so seat B's
input would die at every lock screen, UAC prompt or desktop switch. SYSTEM has
`SE_TCB_PRIVILEGE` and can attach to any desktop in the session. **The comment
claims this survives lock screens. It does not — see §7.**

Launched via `launch_in_session_as_system`: duplicate hydrad's own token, stamp
`TokenSessionId`, `lpDesktop = winsta0\default`. Everything else keeps the user
token via `WTSQueryUserToken` — capture and mirror need the user's graphics
context.

---

## 5. Mode 4 — the IDD path, in full

This is the least-documented and most valuable part. It is ~90% done.

### The goal

A custom IddCx driver gives seat B a **real virtual monitor**, so the session has
a display target that exists regardless of any client window. That coupling —
session has no display unless a client window is asking for updates — is the root
of the entire freeze family that shaped this project. Mode 4 removes it rather
than working around it.

### Remote-session IDDs are explicitly supported

An IDD declares itself a remote-session adapter by setting
`IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER` in `IDDCX_ADAPTER_CAPS.Flags` at
`IddCxAdapterInitAsync`. Introduced in IddCx 1.4; we have 1.11.

**But there is a hard gate.** The OS tracks whether the driver is being loaded
because the RD stack is connecting to a remote session, and fails init in *both*
mismatched cases — flag set for a device the RD stack didn't create, or flag
absent for one it did. **You cannot install a remote IDD and point it at a
session. The RD stack has to instantiate it.**

That is why the RDS protocol provider exists.

### The RDS protocol provider (`HydraProto`)

Based on Microsoft's `Remote-Desktop-Services-Protocol-Sample`, at
`C:\Programs\rdsprov\Sample\TestProtocol_Ext\`. Our two modified files are copied
into `hydra\rdsprov\`: `TestProtocolAPI.cpp`, `WRdsProtocolConnection.cpp`.

Build:

```powershell
cd C:\Programs\rdsprov\Sample; & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' TestProtocol_Ext.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.28000.0 /v:minimal
```

Register with `.\rdsprov-register.ps1 -Register -Apply`. Credentials live in the
listener's own registry key (`Username`/`Password`/`Domain` under
`WinStations\HydraProto`), matching how `RDP-Tcp` stores them — read by the
listener thread when it fires, so set them **before** restarting the service.

**Emergency undo, if the provider takes RDP down:**

```
reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
sc stop TermService
sc start TermService
```

Deleting the key is sufficient — termsrv stops loading the DLL on next start. The
COM registration is inert on its own.

### Status as of the incident

**Working:** the provider creates a real session and logs in with a full desktop,
reliably. Hardware ID `HydraSeat_RemoteIDD_v1` on both sides.

**The open unknown:** the RD stack never asks for the display —
`GetHardwareId` appears never to be called.

`EnableWddmIdd` was the suspect and has been **ruled out by documentation**: it
is termsrv *telling* the provider which mode it operates in, not asking. Its
`[in] Enabled` flag says whether termsrv supports WDDM IDD mode; returning
`S_OK` merely acknowledges. The sample's comment is misleading. `GetHardwareId`
is described as the stack retrieving the ID from you.

So neither call explains the silence, and more reading will not settle it.

### The 08-12 trace, which is now readable

```
EnableWddmIdd(1)        <-- termsrv telling us the mode
AcceptConnection
GetClientData
AuthenticateClientToSession
NotifySessionId
GetInputHandles
GetHardwareId           *** THE STACK IS ASKING FOR THE DRIVER *** count=200
ConnectNotify session=4  (IDD creation starts here)
PreDisconnect reason=17
DisconnectNotify
Close
```

`GetHardwareId` **is** called. Then IDD creation starts and the connection drops.
Consistent with the driver failing to install — which §6 now explains.

### The blocker: error 87

```
iddseat.inf:67:UmdfLibraryVersion = $UMDFVERSION$
iddseat-remote.inf:85:UmdfLibraryVersion = $UMDFVERSION$
```

Both INFs ship the **literal token**. A co-installer handed the string
`$UMDFVERSION$` returns "the parameter is incorrect" = error 87.

`build-driver.ps1` **does not call `stampinf`** — it ends by telling you to. So
the token was never substituted.

**The value is `2.33.0`, NOT 2.35.** `build-driver.ps1` pins 2.33 with the reason
inline: 2.35 ships with WDK 28000, but this OS is build 26100 whose runtime is
2.33, and *requesting 2.35 makes WUDFHost refuse the driver before DriverEntry
runs*.

### Pinned build environment

SDK `10.0.28000.0` · IddCx `1.11` · UMDF **`2.33`** ·
`NTDDI_VERSION=0x0A000010` · cert `CN=HydraTest` (to 2027-07-19) ·
Secure Boot **off** · `testsigning` must read `Yes` (needs a reboot).

```powershell
.\build-driver.ps1            # console IDD  -> dist\driver\
.\build-driver.ps1 -Remote    # remote IDD   -> dist\driver-remote\
```

### Known package numbers

`oem27.inf` — old console package from 4 August, built from an object that never
compiled. Removed offline during the 08-12 recovery.
`oem84.inf` — the remote IDD as staged on 08-11.

### Bugs already found and fixed in this path

- `build-driver.ps1` had **three doubled-quoting bugs**, so `cl.exe` silently
  no-opped and the link reused a stale August `.obj`. `-Remote` had never once
  taken effect and both variants were byte-identical. Fixed.
- `IddCxAdapterDisplayConfigUpdate2` added — without it a remote session's paths
  never activate.
- `iddseat-remote.inf` declares the bare hardware ID with **no `Root\` prefix**,
  because the RD stack creates the devnode rather than `SwDeviceCreate`.
- `UmdfHostProcessSharing=ProcessSharingDisabled`, per IddCx remote guidance.

---

## 6. Dead ends — do not re-run these

| tried | verdict |
|---|---|
| VFIO / hypervisor multiseat | Impossible. Muxless render-only dGPU with no display outputs. |
| ASTER | `mutenx.sys` blocked by April 2026 CI policy. Worked once by deleting `.cip` files from the ESP, with a TrustedInstaller-owned policy left standing. Not survivable across a reset. |
| D3D11 shared textures across sessions | `E_INVALIDARG`. The reason for the whole seqlock design. |
| IDD in session 0 serving another session | Blocked by the same. Remote-session IDD is the supported route. |
| `cursorfence` / `ClipCursor` for the cursor leak | Windows releases the clip on every foreground change. **Real fix was monitor separation** (~8100 px apart: `0,0` and `11340,0`). The reset undid it — restore the gap. |
| VNC instead of RDP for capture | Considered, not pursued. Swaps one capture path for another without touching the underlying coupling. |
| gfx crash: 11 hypotheses | All eliminated. **Actual cause found 08-11**: `update->DesktopResize` was NULL. Fixed. Do not revisit. |
| `seatB_agent` err 5: window station | `fix-winsta0.ps1` proves it was already `WinSta0`. Clean negative. |
| Interception as cause of RDP activation timeouts | Ruled out 08-13. Cause was `TermService` not `type= own`. |
| Two publishers on one ring | Not the cursor/glitch cause; tested 08-13. |

---

## 7. Open problems

**1. PROBLEM 1 — mode 2 random lockups.** Highest priority; teaching depends on
it. Never diagnosed. Run `ON-LOCKUP.md` *before* restarting or the evidence is
gone. First candidate is the `stalled` field (see §3) which has never been
checked during an actual lockup.

**2. `seatB_agent` err 5 at lock screens.** `SendInput` returns
`ERROR_ACCESS_DENIED` when seat B is at a lock screen (`LogonUI.exe` in the
session — runs as **SYSTEM**, so filtering session processes by `teacher` misses
it). The SYSTEM/`SE_TCB_PRIVILEGE` assumption in `hydrad.cpp` ~369 is false.
Seat B cannot unlock itself, because unlocking needs injection into the desktop
injection is blocked from. **Mitigation: stop `teacher` locking.**

**3. Mode 4 — error 87.** See §5. Fix is `2.33.0`, then `stampinf`/`inf2cat`/
sign/`pnputil`. Safety gate first.

**4. PROBLEM 5 — the reboot tax.** `hydrardp` dying leaves the wrapper holding a
session only a reboot clears. ~12 reboots over the project. Fix is a supervisor
that runs `logoff <id>` on *any* exit — clean, crashed or killed.
`SetUnhandledExceptionFilter` handles clean crashes; `TerminateProcess` cannot be
intercepted from inside, so it can't be closed in-process. ~40 lines. Not written.

**5. Audio on modes 2/3.** MSYS2's FreeRDP has no rdpsnd backend, so no Remote
Audio endpoint exists in the seat session and `abcap` has nothing to loopback.
Only mode 1 (mstsc) has audio. Options: rebuild FreeRDP `WITH_WINMM=ON`, try
`/sound:sys:fake`, or give `abcap` a virtual-endpoint fallback.

**6. `display_mode = "client"`.** Set 08-13 after reading `hydrad.cpp` ~695. Not
in `seats.toml`'s comments. Nobody has written down what it does.

---

## 8. INCIDENT-2026-08-12, in one paragraph

Staging `iddseat-remote.inf` + `DenyUnspecified` + `HydraProto` registration
produced a boot loop. Strongest hypothesis: a half-committed CBS transaction
(`revertpendingactions` returned `0x800f082f`). Recovery took a recovery-drive
USB, three BCD undo commands in WinRE, and a "keep files" reset. Root cause is
**permanently unknown** because the reset destroyed `setupapi.dev.log`,
`CBS.log` and `System.evtx`, and they are **not** in `Windows.old`.

Three compounding errors, all now gated by `safety-gate.ps1`:

1. No online undo was written before the change.
2. `bcdedit /set bootstatuspolicy ignoreallfailures` was set on an
   already-failing machine — this suppresses automatic failover *into* WinRE and
   removed the only console route to recovery.
3. The recovery stick from the June ASTER work had never been rebuilt.

Also learned: **in WinRE, `HKLM` is WinPE's own RAM hive.** `reg add`/`reg delete`
there do nothing to the installed system — `reg load HKLM\OFFSYS
D:\Windows\System32\config\SYSTEM` first, and check `Select\Current` for whether
the live set is 001 or 002. Drive letters are reassigned on every WinRE boot.

---

## 9. Rules

- **Read the source, not the summary.** `Select-String -Path .\rdp\hydrardp.c
  -Pattern "<thing>" -Context 0,10`
- **Cheap checks before expensive ones.** Error 87 was a grep behind an OS
  reinstall.
- **Run `safety-gate.ps1` before any boot-risk operation.** Driver install,
  class `UpperFilters`, CI/WDAC, BCD, RDP-Wrapper, ASTER.
- **Never run two modes at once.** Two clients on one session, or two producers
  on one ring, wedges the RDP stack — a reboot, not a restart.
- **Never `Stop-Process -Force` to test shutdown.** That's `TerminateProcess`;
  no user-mode handler can intercept it. Use Ctrl+C.
- **Never extract a whole-tree zip over the working tree.** One stale zip
  downgraded the sources and cost a `git checkout -- .` recovery.
- **Never `bcdedit /set bootstatuspolicy ignoreallfailures`** on a machine you
  might need WinRE on.
- **Capture `setupapi.dev.log`, `CBS.log`, `System.evtx` before any reset.**
- **Machine-specific values belong in the repo docs**, not only in `seats.toml`.
- **Update `STATE.md`.** Its gfx section still sends readers down eleven closed
  hypotheses, and its mode table predates mode 4.

---

## Addendum — ASTER

The dead-ends table above is out of date. ASTER **does** work, using a build sent
directly by their tech support rather than the public download — the public one
ships `mutenx.sys`, which the April 2026 CI policy blocks.

Not currently in use, and not reinstalled after the 2026-08-12 reset. But it is a
working fallback, not a closed door: if Hydra needs to be down for a lesson, this
is the escape route.

Unknowns worth settling before relying on it:
- Where the support build is archived, and whether it survived the reset.
- Whether it still needs Secure Boot off / CI policies removed from the ESP, or
  whether the support build is signed differently.
- Whether it coexists with Interception, or wants exclusive input.

### ASTER installers, located 2026-08-13

`Setup_ASTER2705.exe`  v2.70.5, built 2026-07-01, 60,963,664 bytes
`Setup_ASTER2704.exe`  v2.70.4, built 2026-03-26, 61,147,184 bytes

Both validly signed by IBIK LLC. Neither is distinguishable as a support build --
sequential public releases with ordinary version metadata. The thing support
provided may have been the LICENSE rather than the installer: the original June
problem was a V7 key rejected by the current activation dialog, not a download.

Check the support email before relying on either. Use 2705 (newer).

`vendor/` is gitignored -- 120MB of installers does not belong in the repo.
Copy to the recovery stick, which is FAT32 and has room.
