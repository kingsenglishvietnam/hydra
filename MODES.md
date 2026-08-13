# MODES.md — the four modes

Consolidates the mode sections of `STATE.md` (three modes), the previous
`MODES.md` (two modes) and `STARTUP-MODES.md` (four, written from inference).
Delete the other two mode tables when this lands, or the next reader gets three
answers.

Sources: previous `MODES.md`, `STATE.md`, `build-driver.ps1`, `seats.toml`,
`hydrad.cpp`, and the 2026-08-13 post-reset rebuild. Revision points are marked
**[CHANGED 08-13]** so you can see what this file supersedes.

Machine-level preconditions and rebuild steps: `REBUILD.md`.
Boot-loop post-mortem: `INCIDENT-2026-08-12.md`.

---

## The modes

| | mode 1 | mode 2 | mode 3 | mode 4 |
|---|---|---|---|---|
| client | mstsc | sdl-freerdp | hydrardp | none |
| `display_mode` | `off` | `capture` | `capture` / `client`? | `idd` |
| pixel path | mstsc window is the panel | DDA → ring → mirror | client → ring → mirror | virtual monitor |
| cursor | `cursor_overlay` | composited | composited | native |
| audio | **works** | none — see §Audio | none | untested |
| status | works, freezes when covered | **teach on this** | works, no codec | blocked on error 87 |

**Never run two modes at once.** Two clients on one session, or two producers on
one pixel ring, wedges the RDP stack — and that costs a reboot, not a restart.

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc -ErrorAction SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; query session
```

then `logoff <teacher's ID>`.

---

## Before any mode

Reset silently undoes machine-level settings. Check these first or you debug the
wrong layer:

```powershell
sc.exe qc TermService                    # TYPE : 10 WIN32_OWN_PROCESS  <-- mandatory
Get-Service keyboard, mouse | Select-Object Name, Status               # both Running
.\dist\route_endpoint.exe --list                                       # ids match seats.toml
```

**[CHANGED 08-13] `TermService type= own` is a hard precondition.** Without it
RDP-Wrapper's `ServiceDll` never loads, you get one session, and the symptom is
`ERRCONNECT_ACTIVATION_TIMEOUT` — identical to a wedged stack, but fixed by one
command instead of a reboot. Check it before rebooting.

### Monitor separation — **[CHANGED 08-13] the reset broke this**

The displays were deliberately placed **~8100 px apart** (`0,0` and `11340,0`)
so the desktop is not contiguous and the console cursor physically cannot walk
onto the seat panel. That separation is what retired `cursorfence` and the
`ClipCursor` work — the leak was never a hook problem.

**The reset put them back adjacent: `0,0` and `3240,0`.** Restore the gap in
Settings → System → Display by dragging the seat monitor far to the right (or
above, nudged sideways so only a couple of hundred pixels of edge touch).

Until that is done you will be tempted to fix it with `confine_monitor` /
`clip_console`, which clamps the *console* cursor with a `WH_MOUSE_LL` hook and
traps you on DISPLAY1. Commented out 08-13. Restore the geometry instead.

Current, post-reset:

```
\\.\DISPLAY1   3240x2160 at (0,0)      Surface panel, 200% scaling, primary
\\.\DISPLAY2   1920x1080 at (3240,0)   2770 external — seat B   <-- TOO CLOSE
```

DPI-unaware processes report DISPLAY1 as **926x617** (`mirror --help` does).
`clip_console.exe` and `sdl-freerdp /list:monitor` report true pixels.

### Device bindings — **[CHANGED 08-13] numbers drifted, IDs did not**

| device | hwid substring | seat |
|---|---|---|
| wired Logitech keyboard | `VID_046D&PID_C31C` | console |
| wired Logitech mouse | `VID_046D&PID_C077` | console |
| Surface base keyboard | `Target_KIP&Category_HID` | console |
| wireless keyboard | `VID_1EA7&PID_0066` | **seat B** |
| wireless mouse | `VID_046D&PID_C548` | **seat B** |

Interception device numbers changed completely across the reset — the old seat B
numbers (4/14) now point at the **console** pair. `seats.toml` matches on
hardware-ID substring and needed no edit. Never go back to numeric matching.

Re-learn: `.\dist\seat_router.exe --learn` (service stopped).

### Escape hatch, all modes

Type `Stop-Service Hydra` and press Enter — blind if necessary. Releases input in
~2s. Manual-start, so a reboot also clears it.

---

## Mode 1 — mstsc

`display_mode = "off"`. No capture, no ring: a fullscreen mstsc window *is* the
panel. Input isolation and audio still work.

```powershell
.\hydra-start.ps1 -Client mstsc
```

`Ctrl+Alt+Break` toggles fullscreen, `Ctrl+Alt+Home` summons the connection bar —
either releases a trapped pointer, better than killing the process.

**Why it was superseded:** the panel freezes when the window is minimized or
covered. mstsc sends a Suppress Output PDU and the desktop stops being composed.
Needs `cursor_overlay.exe`, which loses z-order to the Start menu.

**[CHANGED 08-13] Why it is not retired after all:** mstsc is the only client
that gives seat B a *Remote Audio* endpoint. Modes 2 and 3 currently have no
audio at all because of it. The previous MODES.md said there was "no longer a
reason to choose it" — there is now.

---

## Mode 2 — sdl-freerdp. **Teach on this.**

`display_mode = "capture"`. FreeRDP feeds the session; `session_capture` does DDA
inside it, composites the cursor, publishes to the ring; `mirror` draws it.

```powershell
.\hydra-start.ps1
```

One command. Pixel-perfect, because the seat's real desktop is captured rather
than encoded and decoded.

The client window must stay **visible and uncovered** — same Suppress Output
mechanism as mode 1. `hydra-start.ps1` parks it as a 320x200 topmost thumbnail
for exactly that reason. If your cursor reaching the thumbnail moves the seat's
cursor, that window is forwarding input:

```powershell
.\minify-mstsc.ps1 -Process sdl-freerdp -TopMost -Width 320 -Height 200 -Corner BottomLeft
```

By hand, if you must:

```powershell
.\dist\freerdp\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /d: /cert:ignore /f /monitors:2
```

**[CHANGED 08-13]** `/d:` puts login focus on the password field, not the domain
field. `/monitors:2` is FreeRDP's own 1-based enumeration, **not** the Windows
DISPLAY number — confirm with `/list:monitor`.

### mstsc does not read the ring — **[CHANGED 08-13]**

`mirror` draws the ring. Watching the seat through an mstsc window under
`display_mode = "capture"` gives a **working but invisible cursor**: the
composited pointer goes into a buffer nothing is displaying. Not a bug. Cost
several rounds on 08-13.

`mirror.exe B --window` is the **debug** view, not the seat form — no display
target, lands on the console screen, forwards input into seat B by design.

**Open:** PROBLEM 1, random lockups, never diagnosed. Run `ON-LOCKUP.md` before
restarting or the evidence is gone.

---

## Mode 3 — hydrardp, headless. **Development.**

No window anywhere in the capture path, so the freeze that shaped days of this
project is impossible rather than avoided.

```powershell
.\hydra-view.ps1 -Desktop 2
```

One command: clears conflicts, starts the service, opens the client for the
password, **waits for frames to actually exist**, then starts both mirrors and
moves the fullscreen seat view to virtual desktop 2. `Win+Ctrl+Left/Right`
switches.

### By hand — ORDER MATTERS

Service first (it creates the ring), then client, then mirrors. A mirror started
against an empty ring sits at ~7 MB showing nothing.

**Shell 1:**

```powershell
cd C:\Programs\hydra; Start-Service Hydra; Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue; Remove-Item Env:HYDRA_GFX -ErrorAction SilentlyContinue; .\dist\hydrardp.exe B teacher
```

`Stop-Process session_capture` matters — mode 3 must not also have DDA running,
or two producers fight over one ring. `Remove-Item Env:HYDRA_GFX` matters
because a stale `HYDRA_GFX=1` crashes the client.

Wait for `pixel transport opened` **and** publishes climbing.

**Shell 2 — two mirrors:**

```powershell
cd C:\Programs\hydra; Get-Process mirror -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','\\.\DISPLAY2' -WindowStyle Minimized; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','--window','1600x900','56789'
```

Check: `Get-Process mirror | Select-Object Id, MainWindowTitle, WorkingSet` —
two processes, 70–98 MB each.

### HYDRA_GFX selects the CODEC, not on/off

| value | effect |
|---|---|
| `1` | `/gfx` — server picks, usually **H.264. CRASHES.** |
| `RFX` | `/gfx:RFX` — RemoteFX, no H.264 |
| `progressive` | `/gfx:progressive` |
| other | passed through (`AVC420` etc.) |

`1` crashes because this libfreerdp is built `WITH_VAAPI_H264_ENCODING=ON`, which
the library warns is experimental. Null call immediately after the pipeline
attaches — half-initialised codec. FreeRDP issue 12221 is the same shape.

### **[CHANGED 08-11] The gfx crash is FIXED**

`STATE.md` still ends at "STOPPED at ten eliminated candidates" and the previous
`MODES.md` says three hypotheses were tried. Both are stale.

Found via `hydra_veh.c` (first-chance VEH resolving `[rsp+00]` to module+RVA):
crash was `call *0x68(%r14)` at `libfreerdp3.dll+0xDCB4B`, confirmed by `offs.c`
as `offsetof(rdpUpdate, DesktopResize) == 0x068`. **`update->DesktopResize` was
NULL.** Fix: `hydra_desktop_resize()` assigned in `hydra_post_connect`. Tagged
`gfx-working`.

Two further fixes the same session: throttle check moved to the top of
`hydra_end_paint` (CPU ~200% → ~93s total), and publishing moved to fire from the
gfx `EndFrame` callback rather than a blind 16 ms timer, which ended the
glitching.

**Do not re-run the eleven eliminated hypotheses.** They are closed.

### **[CHANGED 08-13] Cursor tracking works**

Previous MODES.md says `agent:B` is "designed, not built". It is built, running,
and publishing — `curSeq` advances in `hydra-shm.ps1`. RDP still sends no pointer
position to a client that generates no input; `agent:B` fills that gap by
publishing `GetCursorPos` into `pixHdr->curX/curY/curSeq`.

### **[CHANGED 08-13] Survives a reset with no rebuild**

`hydrardp.exe` and `hydrardp.exe.GOOD-20260811-1607` are byte-identical, and the
FreeRDP DLLs are already beside them in `dist\`. Mode 3 ran post-reset with no
MSYS2 and no compiler.

### Open

- **`display_mode = "client"`** was set on 08-13 after reading `hydrad.cpp` ~695.
  Not in `seats.toml`'s comments, which list only `capture`, `off`, `idd`.
  Someone needs to write down what it does.
- PROBLEM 5, the reboot tax: `hydrardp` dying leaves the wrapper holding a
  session only a reboot clears. Fix is a supervisor that runs `logoff <id>` on
  *any* exit — clean, crashed or killed. ~40 lines. Not written.

---

## Mode 4 — kernel mode (IddCx driver + HydraProto)

`display_mode = "idd"`. A custom IddCx indirect display driver gives seat B a
real virtual monitor and the `HydraProto` protocol provider hands it to
`termsrv`. No DDA, no ring, no client in the pixel path. Only mode that creates
the `SWD\HYDRA` device.

**~90% complete. Blocked on error 87.** Interrupted by INCIDENT-2026-08-12 and
not properly documented elsewhere — this section is the record.

### The blocker, confirmed 08-13

```
iddseat.inf:67:UmdfLibraryVersion = $UMDFVERSION$
iddseat-remote.inf:85:UmdfLibraryVersion = $UMDFVERSION$
```

Both INFs ship the **literal token**. A co-installer handed the string
`$UMDFVERSION$` returns "the parameter is incorrect" — error 87.

Matches the 08-12 provider trace: `GetHardwareId *** THE STACK IS ASKING FOR THE
DRIVER ***`, then `ConnectNotify session=4`, then `PreDisconnect reason=17`. The
stack asked, the driver could not install, the connection dropped.

`build-driver.ps1` **does not call `stampinf`** — it ends by telling you to. So
the token was never substituted, rather than substituted wrongly.

### The value is 2.33.0 — NOT 2.35

`build-driver.ps1` pins it with the reason inline:

```powershell
$umdf = '2.33'   # 2.35 ships with WDK 28000 but this OS is build 26100 (24H2),
                 # whose runtime is 2.33 -- requesting 2.35 makes WUDFHost refuse
                 # the driver before DriverEntry runs
```

`UmdfLibraryVersion` must match what the DLL was linked against. **2.35 fails
before `DriverEntry`**, which would look like an entirely different bug.

Since `build-driver.ps1` copies the INFs verbatim rather than regenerating them,
hardcoding `2.33.0` is safe — there is nothing to overwrite it.

### Pinned build environment

| | |
|---|---|
| kit root | `C:\Program Files (x86)\Windows Kits\10` |
| SDK | `10.0.28000.0` |
| IddCx | `1.11` |
| UMDF | **`2.33`** |
| `NTDDI_VERSION` | `0x0A000010` |
| signing cert | `CN=HydraTest`, valid to 2027-07-19 |

```powershell
.\build-driver.ps1            # console IDD  -> dist\driver\
.\build-driver.ps1 -Remote    # remote-session IDD -> dist\driver-remote\
```

### Path to finish

1. Fix `UmdfLibraryVersion` → `2.33.0` in both INFs.
2. `.\build-driver.ps1`
3. `stampinf`, then `inf2cat` — the existing `iddseat.cat` is dated 4 August and
   was built from a stale object, so it signs the wrong thing.
4. `.\sign-driver.ps1`
5. `bcdedit /enum {current} | Select-String testsigning` → must read `Yes`.
   Secure Boot is off, so `bcdedit /set testsigning on` is accepted.
6. `pnputil /add-driver` — **the boot-risk step.**
7. `.\rdsprov-register.ps1`

### Safety gate — mandatory before step 6

```powershell
.\safety-gate.ps1 -Label "iddseat-mode4"
```

Three levels of undo; it refuses if any is missing.

1. **cwd** — driver-store snapshot + generated `undo-online.ps1` that diffs
   before/after and removes the delta, plus captured class filters.
2. **WinRE** — `reagentc` Enabled, and `bootstatuspolicy` **not**
   `ignoreallfailures`. Setting that on a failing machine is what removed the
   route into WinRE on 08-12.
3. **USB** — tested bootable stick, present, with `UNDO-OFFLINE.txt` on it.

Level N is only a real undo if level N+1 exists. On 08-12 all three were gone at
once.

### Known dead end

The **remote-session** IDD variant was tested and **cannot work under
RDP-Wrapper** (`seats.toml` records this). Mode 4 is the console-side IDD.

---

## Audio

`abcap` (in the seat session) → shared ring → `abren` (console) → monitor
endpoint. Bypasses the RDP channel so audio does not lag video — the channel
buffers far more than DDA capture does.

### **[CHANGED 08-13] Endpoint GUIDs were reissued by the reset**

| device | old | current |
|---|---|---|
| Intel Display Audio (monitor) | `623f2512` | **`3012a3df`** |
| Realtek (laptop speakers, seat 1) | `548a2a1a` | **`969db10d`** |

`seats.toml` `audio_bridge`, its comment table, and `audiotest.exe`'s built-in
help text all named the old ones. Symptom in `abren_B.log`:

```
[abridge] no render endpoint matching "623f2512" -- retry 340
```

Verify after any driver reinstall: `.\dist\route_endpoint.exe --list`

### **[CHANGED 08-13] Modes 2 and 3 have no audio**

The bridge bypasses the RDP *transport*, not the *source*. `abcap` does WASAPI
loopback on the seat session's default render endpoint — which on
`audiomode:i:0` is the *Remote Audio* device the RDP client negotiates into
existence.

MSYS2's FreeRDP has **no rdpsnd backend** (`[static] Loaded fake backend`;
`pacman -Ql mingw-w64-x86_64-freerdp` ships headers only). No backend → no
channel → no endpoint → `abcap` fails `hr=0x80070490` forever.

So audio currently requires mode 1. Unresolved: rebuild FreeRDP with
`WITH_WINMM=ON` (a fake backend is fine — the point is negotiating rdpsnd so the
endpoint exists), try `/sound:sys:fake`, or give `abcap` a virtual-endpoint
fallback.

Verified working 08-13: `audiotest.exe 0 3012a3df` → FULL SUCCESS, peak 0.987,
rendered to monitor. PID `0` is whole-session capture and is the right test — a
specific PID captures that process only. **Mute mstsc in the console volume
mixer** or whole-session capture takes its playback too and you get feedback.

---

## Symptoms

| symptom | cause |
|---|---|
| `ERRCONNECT_ACTIVATION_TIMEOUT` | **[CHANGED 08-13]** check `sc qc TermService` for `type= own` FIRST — one command. Otherwise the stack is wedged: reboot. Confirm it is not us: `C:\msys64\mingw64\bin\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /cert:ignore` failing the same way means the wrapper. |
| `ERRCONNECT_LOGON_FAILURE` | Wrong password. Echo is off, so a typo is invisible. |
| `LOGON_MSG_BUMP_OPTIONS` then logoff ~30s | RDP-Wrapper not active. `type= own`. |
| `no pixel ring` | Service not running. `Start-Service Hydra` |
| Mirrors ~7 MB, blank | Started before the client was publishing. |
| White box on the panel | Stale ring, publisher died. Publisher first, mirror second. |
| Cursor moves but is invisible | **[CHANGED 08-13]** you are watching mstsc, which does not read the ring. Use `mirror`. |
| `SendInput ... (err 5)` in `agent_B.log` | Seat B is at a lock screen (`LogonUI.exe` in the session — runs as SYSTEM, so filtering by `teacher` misses it). See §Open. |
| `[router] kbd=0 mouse=0` | **[CHANGED 08-13]** cosmetic. Prints the unused numeric fallback fields; matching happens per-event on hardware ID. |
| `no render endpoint matching ...` | Stale audio GUID. `route_endpoint.exe --list` |
| Video glitchy, no gfx | Plain bitmap updates, no codec. Expected. |
| `LNK1104` on build | The exe is running. Kill it first. |

---

## Open — `seatB_agent` err 5

`SendInput` returns `ERROR_ACCESS_DENIED` when seat B's session is at a lock
screen. `hydrad.cpp` ~369 asserts SYSTEM + `SE_TCB_PRIVILEGE` lets the agent
attach to any desktop including Winlogon. **It does not hold.**

Ruled out 08-13: not UIPI (integrity `0x4000`); not the window station
(`fix-winsta0.ps1` added an explicit `SetProcessWindowStation` and it logs
`WinSta0` — already correct); not mode 3 or headless sessions (fails identically
under `sdl-freerdp`); not a respawn loop (one stable PID).

Seat B cannot unlock itself, because unlocking needs injection into the desktop
injection is blocked from. **Mitigation: stop `teacher` locking** — power and
screensaver settings on that account.

---

## Do not

- Run two modes at once.
- Use `HYDRA_GFX=1`.
- Test clean shutdown with `Stop-Process -Force` — that is `TerminateProcess`,
  no user-mode handler can intercept it. Use **Ctrl+C**.
- Extract a whole-tree zip over the working tree. One stale zip downgraded the
  sources and cost a `git checkout -- .` recovery. Individual files only.
- Patch on a hunch. The gfx crash took eleven wrong hypotheses and one source
  read. Error 87 was findable by grep and sat behind an OS reinstall.
