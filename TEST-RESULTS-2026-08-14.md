# TEST-RESULTS-2026-08-14.md

Three tests run after the machine was rebuilt following INCIDENT-2026-08-12.
All three produced answers. Two closed open questions that had been inference
since 08-11.

---

## TEST 1 — monitor separation (PROBLEM 4)

**Result: fixed.**

The seat monitor was adjacent to the console monitor's right edge, so the
console pointer walked onto it constantly. That crossing is what made the client
thumbnail forward input into seat B — the long-standing "glitchy cursor" in
modes 2 and 3 was **input forwarding**, not `agent:B`, not err 5, not the codec.

`MODES.md` had said so all along:

> If your cursor reaching that thumbnail moves the seat's cursor, the window is
> forwarding your input — move it out of the way.

Two changes:

1. Seat monitor moved **above** the console monitor in Display settings.
2. `hydra-start.ps1` client thumbnail moved from `-Corner TopRight` to
   `-Corner BottomLeft` — TopRight sat directly on the crossing path.

Nathan's verdict: **"basically perfect."**

### Residual, and why it was left

`clip_console.exe` (DPI-aware, real pixels) reports:

```
monitor 0: (0,0)-(3240,2160)  \\.\DISPLAY1
monitor 1: (1310,-1080)-(3230,0)  \\.\DISPLAY2
```

DISPLAY2's X range sits **entirely inside** DISPLAY1's, so the full width is
still technically a crossing — 1920px of it. In practice a deliberate upward
movement rather than the constant sideways drift it replaced.

Belt-and-braces applied instead of further dragging: **`confine_monitor =
'\\.\DISPLAY1'` re-enabled** in `seats.toml`. `clip_console` clamps the console
cursor with a `WH_MOUSE_LL` hook so it slides along the edge. Previously retired
as "the wrong fix" — but it was wrong only while the monitors were adjacent and
it was doing the whole job alone. With separation as the primary fix it is a
reasonable second layer.

Escape hatch if it ever clamps somewhere useless: type `Stop-Service Hydra`
blind and press Enter. Releases in ~2s.

### Note

`hydra-start.ps1` **already starts the mirror.** Running
`.\dist\mirror.exe B \\.\DISPLAY2` afterwards was an extra step, and two mirrors
on one panel fight each other. Same for `hydra-view.ps1`, which starts two by
design (fullscreen + windowed).

### PowerShell reports the wrong geometry

`[System.Windows.Forms.Screen]::AllScreens` is not Per-Monitor-V2 aware and
reported DISPLAY1 as **926x617** — the 200%-scaled figure. Real size is
3240x2160. `clip_console.exe` and `sdl-freerdp /list:monitor` report true pixels.
**Do not do arithmetic on the scaled numbers.**

---

## TEST 2 — the video glitch A/B

**Result: the producer, not the render path. Mode 3 only.**

`hydra-view.ps1` starts two mirrors from the **same ring** — fullscreen on
DISPLAY2 and a 1600x900 window. Same frames, two present paths.

Observation with `HYDRA_GFX=RFX`, playing YouTube in the seat:

- **Both mirrors glitch identically**, video and cursor
- **Mode 2 is perfect** under the same conditions

Both identical rules out `mirror`'s present to DISPLAY2, the ring itself, and
the upload. Mode 2 being clean rules out anything shared, since mode 2 uses the
same ring and the same `mirror` with a different producer (`session_capture` DDA
rather than `hydrardp`).

**So the fault is entirely inside `hydrardp`'s publish/composite path.**

Two theories were burned reaching this and neither survived contact: the Intel
GPU driver version (30.0.101.3118, restored by the reset) and the codec setting.
Both were assertions made before measuring. The A/B took one look.

### Live ring measurement during the glitch

```
15:xx:37  pix seq=3878  cur (1059,708) curSeq=2941
15:xx:41  pix seq=4644  cur (805,206)  curSeq=3186
```

`seq` +766 over 4s. `seq` is a **seqlock** and increments twice per publish
(`hydrardp.c:399` — "even: snapshot complete"), so that is **~96 publishes/sec**.
`curSeq` +245 ≈ **61/sec**, matching the ~62/sec `RESUME-2026-08-11` recorded as
normal for `agent:B`. Positions arrive correctly and movement is tracked.

96/sec is above the 16ms gate at `hydrardp.c:275` (~62/sec ceiling). There are
four paths into `hydra_end_paint` — the chained EndPaint per damaged region,
`hydra_gfx_end_frame`, the `g_curMoved` republish at 1123, and the timer at 1135.
Either `GetTickCount` granularity or `lastPublish` not being updated on every
path. **Not yet investigated.**

A "two of three frames carry a stale cursor position" theory was raised and is
**wrong**: `hydra_composite_pointer` (line 561) reads `curX`/`curY` from the
shared header at composite time, immediately before the seqlock closes.

### Status

Mode 2 is the teaching mode and is unaffected. Mode 3 is the development client.
Not blocking.

---

## TEST 3 — driverless mode 4

**Result: NO. The IDD is required, and not for the reason assumed.**

### The hypothesis

The RDS provider already creates a real logged-in Windows session with no RDP in
the path (`ec995bb`, tag `rdsprov-session`). Modes 1–3 already extract pixels
from a session with **no driver** via DDA → ring → `mirror`. So: does a
provider-created session have a duplicatable display? If yes, mode 4 needs no
driver at all — no `iddseat.dll`, no `0xD000000D`, no signing, no `pnputil`,
nothing in the path that can prevent a boot.

### Confirmed on the way

**The provider does NOT need RDP-Wrapper.** With `ServiceDll` at stock
`termsrv.dll`, no wrapper loaded:

```
>console        user   1  Active
 hydraproto#0          4  Conn
 hydraproto        65536  Listen
```

Console session and provider session concurrently. `NEXT-STEPS.md` listed this
as an open question and called its own answer "inference, not fact." **It is now
fact.** Modes 1–3 and mode 4 are alternative stacks, not competitors for one
session slot.

**XDDM is a dead end.** `WRdsProtocolConnection.cpp:364`: *"If this returns
false, it will load XDDM drivers (legacy)."* XDDM is kernel-mode and deprecated
since Windows 8 — strictly harder than the IddCx problem already open.
`GetVideoHandle` is instrumented with `"<-- XDDM path, should NOT fire under
IDD"` and has never fired.

### The failure

Both driver packages unstaged (`oem222.inf`, `oem223.inf` removed), so nothing
could fail at IDD creation. Provider triggered.

```
15:26:12.542 EnableWddmIdd(1)
15:26:12.542 AcceptConnection
15:26:12.542 GetClientData
15:26:12.542 AuthenticateClientToSession
15:26:12.587 NotifySessionId
15:26:12.587 GetInputHandles
15:26:12.587 GetHardwareId  count=200
15:26:12.589 ConnectNotify session=3
15:26:12.659 NotifyCommandProcessCreated session=3
                                                    <-- nothing further
```

Sampled at 8s, 16s, 24s — **identical every time**:

```
session 3 : 7 procs, explorer=False, LogonUI=True
```

Processes: `csrss`, `ctfmon`, `dwm` (as DWM-3), `fontdrvhost`, `LogonUI`,
`NVDisplay.Container`, `winlogon`. A compositor, and no shell, indefinitely.

### The evidence that names it

**Security 4624 — logon SUCCEEDED:**

```
Account Name:   teacher
Logon Type:     10          (RemoteInteractive)
Logon Process:  User32
```

No 4625. Authentication is not the problem.

**Winlogon 7001 — ABSENT for that logon.** The 7001s at 15:22:24 and 15:23:59
are the mstsc profile-warming logins. The 15:26:13 provider logon produced none.
7001 is winlogon signalling a completed user logon — the step immediately before
the shell starts.

So: **authentication succeeds, winlogon creates the shell process, and then
stops before completing the logon.** Nothing fails. Nothing is logged. It waits.

### Interpretation

Winlogon appears to need a display target to finish the desktop switch. `dwm`
runs but has no output device, there is nowhere to put the interactive desktop,
and the state machine parks.

### Eliminated on the way

- **Credentials.** 4624 success, no 4625. `Username=teacher`, `Domain=''`,
  password correct.
- **Provider timeout.** Two runs: one ended `PreDisconnect reason=12` at 33s,
  one was still alive at 32s with no PreDisconnect at all. Same stalled state.
- **Cold profile.** Warmed `teacher`'s profile via mstsc first (full desktop,
  clean sign-out), then retriggered. Identical result.
- **A staged driver interfering.** Both packages removed and verified absent.

### Consequence

**`0xD000000D` is back on the critical path.** Mode 4 is one problem, not two.
The earlier run in which `teacher` reached Active must have had a console IDD
staged — which is consistent with `RESUME-2026-08-12`'s good run.

Next instrument, never used: the WUDF **framework verifier** (`WdfVerifier.exe`,
WDK), which logs *why* a driver was rejected rather than only that it was. Or
build Microsoft's own IddCx sample — if that also fails to load, the problem is
environmental rather than in `iddseat.cpp`.

---

## Machine state at end of session

Cleanup applied and verified:

- Provider unregistered, COM class unregistered, listener key removed
- `C:\TestProtocol\createconnection.txt` removed (present at boot = a connection
  fires on its own)
- `ServiceDll` restored to `rdpwrap.dll`; both `rdpwrap.dll` and `termsrv.dll`
  confirmed loaded into `TermService`
- Both IDD packages unstaged — mode 4 work restarts from `pnputil /add-driver`
  using `dist\driver-remote` (IddCx 1.10, UMDF 2.33, WDK 26100, CN=HydraTest)

Modes 1 and 2 working. Mode 3 runs with the glitch above.

### Traps confirmed again

- `rdsprov-register.ps1 -Register` recreates the listener key **EMPTY** every
  time. Credentials must be re-set after every register. Sixth occurrence.
- Stale `SessionId` devnodes in `CM_PROB_FAILED_ADD` block retriggering; reboot
  between attempts.
- Repeated half-completed logons leave `teacher` stuck; a reboot clears it.
- `pnputil /delete-driver` prints the entire driver store on failure. Filter it:
  `(pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n" | Where-Object { $_ -match 'iddseat' }`
