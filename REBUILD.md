# REBUILD.md — bringing Hydra back on a freshly reset machine

Written 2026-08-13, the day after INCIDENT-2026-08-12, on the machine that had
just been through **Reset this PC → Keep my files**.

The reset took every application and several machine-wide settings that nothing
in this repo records. Rediscovering them cost most of a day. This file is that
day, written down.

Read `INCIDENT-2026-08-12.md` for the boot-loop post-mortem. This file is what
comes *after* the machine boots again.

---

## 0. What survives a reset, what doesn't

| Survives | Gone |
|---|---|
| `C:\Programs\hydra` (source **and** `dist\` binaries) | VS Build Tools |
| `C:\Scripts` | git, MSYS2, Notepad++, Python |
| `C:\msys64` tree (files stay, registration goes) | RDP-Wrapper |
| PowerShell 7 (per-user install) | Interception |
| PowerShell profiles (OneDrive-synced) | `sc config TermService type= own` |
| User documents | Audio endpoint GUIDs (reissued) |
| | Display arrangement / scaling |
| | `C:\Windows\Logs`, `setupapi.dev.log`, CBS logs |

**`dist\` surviving is the important one.** Every compiled binary was intact,
including `hydrardp.exe` matching its `.GOOD` backup byte for byte. Mode 3 ran
with no rebuild at all. Do not assume a reset means recompiling.

**Logs do not survive.** Reset clears `C:\Windows\Logs\CBS` and
`C:\Windows\INF\setupapi.dev.log`, and they are *not* in `C:\Windows.old`.
Copy them off **before** resetting or they are gone permanently. That is why the
2026-08-12 root cause is still unknown.

---

## 1. Toolchain, in order

```powershell
winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements
winget install --id Notepad++.Notepad++ -e
winget install --id MSYS2.MSYS2 -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended"
```

Notes earned the hard way:

- Use `--passive`, **not** `--quiet`. Quiet gives no progress display for a
  ~7GB install and you cannot tell it from a hang.
- VS downloads into `C:\ProgramData\Microsoft\VisualStudio\Packages` first and
  only writes to `Program Files` at the install phase. Watching `Program Files`
  size makes a working install look frozen.
- MSYS2 refuses to install over an existing `C:\msys64` and exits with code 1.
  Rename it first.
- `install-shortcut.ps1` throws until `vcvars64.bat` exists — by design. Run it
  **after** Build Tools, and it re-discovers the path on this machine rather
  than trusting the old shortcut. "Hydra Shell" is a `.lnk` (user data, so it
  survives the reset) pointing at a `vcvars64.bat` that does not — which
  produces `The system cannot find the path specified.` and a bare
  `C:\Windows\System32>` prompt.

---

## 2. RDP-Wrapper — **`type= own` is the thing everyone forgets**

```powershell
Add-MpPreference -ExclusionPath 'C:\Program Files\RDP Wrapper'
mkdir C:\Temp\rdpwrap -Force; cd C:\Temp\rdpwrap
Invoke-WebRequest 'https://github.com/stascorp/rdpwrap/releases/download/v1.6.2/RDPWrap-v1.6.2.zip' -OutFile RDPWrap.zip
Expand-Archive RDPWrap.zip -DestinationPath . -Force
.\install.bat
Invoke-WebRequest 'https://raw.githubusercontent.com/sebaxakerhtc/rdpwrap.ini/master/rdpwrap.ini' -OutFile 'C:\Program Files\RDP Wrapper\rdpwrap.ini'
sc.exe config TermService type= own
Restart-Computer -Force
```

**`sc config TermService type= own` is mandatory and is silently undone by the
reset.** Without it `TermService` runs in a shared `svchost` and the replaced
`ServiceDll` is never loaded — correct files, correct registry, stock
single-session behaviour. Symptom is `ERRCONNECT_ACTIVATION_TIMEOUT` after ~14s.
Space after `type=` is required by `sc.exe`.

Never append a single build section to an existing `rdpwrap.ini`. Recent
`termsrv` builds need new patchcodes; replace the whole file.

### Verifying it — RDPConf lies

**RDPConf's "Service state: running" does not say whether the wrapper is
loaded.** Do not read it as a diagnostic. The ground truth:

```powershell
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' ServiceDll
$p = (Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId
Get-Process -Id $p -Module | Where-Object ModuleName -match 'rdpwrap|termsrv' | Select-Object ModuleName, FileName
```

Both `rdpwrap.dll` and `termsrv.dll` loaded = working. rdpwrap loads the real
termsrv behind itself, so seeing both is correct, not a conflict.

`PathName` stays `svchost.exe -k TerminalService` wrapped or not. It is not a test.

RDPConf itself renders unreadably on the 200%-scaled Surface panel. Properties →
Compatibility → high-DPI → override scaling by **Application** (System does not work).

Verified working: **termsrv 10.0.26100.8115**, supported by the sebaxakerhtc ini
since 2026-04-03.

---

## 3. Interception

```powershell
dism /online /get-drivers /format:table > C:\Programs\hydra\driver-inventory-pre-interception.txt
& "C:\Programs\Interception\command line installer\install-interception.exe" /install
```

Path has **spaces**: `command line installer\`, not `command_line\`.

`keyboard.sys` / `mouse.sys` are embedded as resources in the installer and
extracted to `System32\drivers` at install time. Their absence from the
distribution tree is normal.

**Verify before rebooting.** This is the whole boot risk of the step:

```powershell
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96B-E325-11CE-BFC1-08002BE10318}" /v UpperFilters
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96F-E325-11CE-BFC1-08002BE10318}" /v UpperFilters
Get-ChildItem C:\Windows\System32\drivers\keyboard.sys, C:\Windows\System32\drivers\mouse.sys
```

Expect exactly `keyboard\0kbdclass` and `mouse\0mouclass`, and both `.sys`
present. A filter entry naming a driver not on disk is the boot-stopping
combination.

### Known-good values — write these somewhere off-machine

```
{4D36E96B-E325-11CE-BFC1-08002BE10318}  UpperFilters = kbdclass    (stock, no Interception)
{4D36E96F-E325-11CE-BFC1-08002BE10318}  UpperFilters = mouclass    (stock, no Interception)
```

`reg query` prints `REG_MULTI_SZ` entries separated by a literal `\0`, so
`keyboard\0kbdclass` is two entries, not a typo.

### Interception is NOT the cause of RDP activation timeouts

Tested 2026-08-13: uninstalled, activation worked; reinstalled and rebooted,
activation still worked. The timeouts were `type= own` missing plus a wedged
session. Do not chase this again.

---

## 4. Device numbers drift — hardware IDs do not

Interception device numbers **changed completely** across the reset:

| | before | after |
|---|---|---|
| console keyboard `VID_046D&PID_C31C` | — | dev 4 |
| console mouse `VID_046D&PID_C077` | — | dev 14 |
| seat B keyboard `VID_1EA7&PID_0066` | 4 | dev 6 |
| seat B mouse `VID_046D&PID_C548` | 14 | dev 12 |

The old numbers now point at the **console** pair. Starting the service with
numeric matching would have handed your own keyboard and mouse to seat B.

`seats.toml` matches by hardware-ID substring and needed **no change**. This is
the single design decision that saved the most time on rebuild day. Keep it.

Re-learn with `.\dist\seat_router.exe --learn` (service stopped).

### `kbd=0 mouse=0` in router.log is cosmetic

`[router] seat B: kbd=0 mouse=0` prints `g_seat[i].kbd` / `.mouse` — the
**numeric fallback** fields, which stay 0 when `kbd_id`/`mouse_id` are set.
Matching happens per-event in `match_kbd`/`match_mouse` (seat_router.c ~602/611).
Zero here does not mean no devices. Cost an hour on 2026-08-13.

Also: Surface base keyboard enumerates as `HID\Target_KIP&Category_HID&Col01`
and does pass through Interception.

---

## 5. Audio — **endpoint GUIDs are reissued by the reset**

This is the one that looks like a broken pipeline and is a one-line config fix.

```powershell
.\dist\route_endpoint.exe --list
```

| device | old id | current id (2026-08-13) |
|---|---|---|
| Intel Display Audio (monitor) | `623f2512` | **`3012a3df`** |
| Realtek (laptop speakers, seat 1) | `548a2a1a` | **`969db10d`** |
| CABLE Input (VB-CABLE, not installed) | `0329839f` | n/a |

`seats.toml` `audio_bridge` and the ID table in its comments both need updating.
`audiotest.exe`'s built-in help text also still names `623f2512` — it is stale
documentation inside the tool, fix it when convenient.

Symptom of a stale id, in `abren_B.log`:

```
[abridge] no render endpoint matching "623f2512" -- retry 340
```

### The bridge bypasses the RDP *transport*, not the *source*

`abcap` does WASAPI loopback on the seat session's **default render endpoint**.
On `audiomode:i:0` the seat has no real hardware — that endpoint is the *Remote
Audio* device the RDP client negotiates into existence.

**MSYS2's FreeRDP has no rdpsnd backend** (`[static] Loaded fake backend for
rdpsnd`; `pacman -Ql mingw-w64-x86_64-freerdp` ships headers only). No backend →
no channel negotiated → no endpoint in the session → `abcap` fails with
`hr=0x80070490` (ELEMENT_NOT_FOUND) forever.

So: **audio currently requires mstsc.** `sdl-freerdp` and `hydrardp` cannot
supply the endpoint. This is an architectural conflict — mode 2's panel-freeze
fix (FreeRDP) and mode 2's audio (mstsc) presently want different clients.

Options, none yet taken:
1. Rebuild FreeRDP with `WITH_WINMM=ON` — the backend can stay fake, the point
   is that rdpsnd gets negotiated so the endpoint exists.
2. Test `/sound:sys:fake` to force negotiation without a rebuild.
3. Have `abcap` fall back to a virtual endpoint when the session has none.

### Verified working

`audiotest.exe 0 3012a3df` → FULL SUCCESS, peak 0.987, rendered to monitor.
PID `0` triggers whole-session capture and is the right test — a specific PID
captures that process only, so `explorer.exe` reads 0.000 and means nothing.

Whole-session capture takes mstsc's own playback too. **Mute mstsc in the
console volume mixer** or you get feedback / double audio.

---

## 6. Display — mstsc is not the seat display

**`display_mode = "capture"` composites the cursor into the shared-memory ring.
`mirror` draws the ring. mstsc does not read the ring at all.**

Watching the seat through an mstsc window therefore gives a **working but
invisible cursor** — the composited pointer is being drawn into a buffer nothing
is displaying. This is not a bug. Cost several rounds on 2026-08-13.

Correct arrangement:

```powershell
.\dist\mirror.exe B \\.\DISPLAY2      # <-- the seat display
```

mstsc's role is creating the session and providing the audio endpoint. Minimise
it rather than closing it (closing may end the session).

`mirror.exe B --window` is the debug view, **not** the seat form. It has no
display target, lands on the console screen, and forwards input into seat B by
design — which reads as a cursor leak.

### Geometry

```
\\.\DISPLAY1   3240x2160 at (0,0)      Surface panel, 200% scaling, primary
\\.\DISPLAY2   1920x1080 at (3240,0)   2770 external
```

**DPI-unaware processes see DISPLAY1 as 926x617.** `mirror --help` reports the
scaled figure; `sdl-freerdp /list:monitor` and `clip_console.exe` (which opts
into Per-Monitor-V2) report the true 3240x2160. Do not do pixel maths on the
scaled number.

FreeRDP monitor indices are 1-based and its own enumeration:
`sdl-freerdp /list:monitor` → the 2770 is **index 2**. Do not guess.

Placing a window without a mouse:

```powershell
Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);'
[W.U]::SetWindowPos((Get-Process mstsc | Select-Object -First 1).MainWindowHandle, [IntPtr]::Zero, 3240, 0, 1920, 1080, 0x0040)
```

### `confine_monitor` / clip_console

`confine_monitor = '\\.\DISPLAY1'` makes `clip_console` clamp the **console**
cursor to the Surface panel via a `WH_MOUSE_LL` hook. Working as designed, but
it stops a solo operator reaching DISPLAY2. Comment it out when there is no real
second user — `hydrad` then does not launch `clip` at all.

`Stop-Process -Force` on `clip_console` skips its cleanup. Its header says Ctrl+C
releases and a crash lets the OS remove the hook; a forced kill is neither.

---

## 7. `seatB_agent` err 5 — OPEN

`SendInput` returns `ERROR_ACCESS_DENIED (5)`, intermittently, when seat B's
session is at a **lock screen** (`LogonUI.exe` present in the session).

Design intent (hydrad.cpp ~369): the agent runs as SYSTEM via
`launch_in_session_as_system` specifically so `SE_TCB_PRIVILEGE` lets it attach
to any desktop including Winlogon. **It does not hold.** That assumption is
false and the comment should be corrected.

Ruled out on 2026-08-13:

- **Not UIPI.** Diagnostic reads `our integrity=0x4000` (System).
- **Not the window station.** `fix-winsta0.ps1` added an explicit
  `OpenWindowStationW(L"WinSta0")` + `SetProcessWindowStation()` at the top of
  `main()`, which logs `[agent] window station: WinSta0` — it was already
  correct. Patch is harmless and the log line is worth keeping as a permanent
  rule-out.
- **Not mode 3 / headless sessions.** Fails identically under `sdl-freerdp`.
- **Not a respawn loop.** One PID, stable.

Remaining: seat B cannot unlock itself, because unlocking needs injection into
the desktop injection is blocked from. Every lock is a manual console
intervention. **Practical mitigation: stop `teacher` locking** (power/screensaver
settings on that account).

Note `LogonUI.exe` runs as **SYSTEM**, so filtering session processes by
`UserName -like '*teacher*'` will not find it. Cost several rounds.

---

## 8. Error 87 (IDD driver) — CONFIRMED, UNFIXED

```powershell
Select-String -Path C:\Programs\hydra\iddseat\iddseat.inf, C:\Programs\hydra\iddseat\iddseat-remote.inf -Pattern 'UmdfLibraryVersion'
```

Both INFs ship the **literal token**:

```
iddseat.inf:67:UmdfLibraryVersion = $UMDFVERSION$
iddseat-remote.inf:85:UmdfLibraryVersion = $UMDFVERSION$
```

`build-driver.ps1` calls `stampinf` without the flag that substitutes it. Handing
a co-installer the string `$UMDFVERSION$` produces exactly "the parameter is
incorrect". Fix is substitution to `2.35.0`, matching what `build-driver.ps1`
already pins.

**Not yet applied** — the fix leads to staging a driver package, which is the
category that caused INCIDENT-2026-08-12. Safety gate first.

This was findable by a read-only grep at any point. It was blocker #5 in a chain
that cost an OS reinstall. **Run the cheap checks before the expensive ones.**

---

## 9. Startup order that works

```powershell
sc.exe qc TermService                          # TYPE : 10 WIN32_OWN_PROCESS
cd C:\Programs\hydra
mstsc /v:127.0.0.2 /w:1920 /h:1080             # creates session + audio endpoint
query session                                  # teacher must read Active
Start-Service Hydra
Start-Sleep 5; .\dist\hydractl.exe status
.\dist\mirror.exe B \\.\DISPLAY2               # the actual seat display
```

Mute `mstsc.exe` in the console volume mixer.

Healthy status: `router`, `abcap:B`, `abren:B`, `agent:B`, `capture:B` all
running (`clip` only if `confine_monitor` is set).

Escape hatch, unchanged: type `Stop-Service Hydra` and press Enter. Keyboard
reaches the console even when the mouse does not.

### Mode 3

`hydrardp.exe` runs with **no rebuild and no MSYS2** — its DLLs are already
beside it in `dist\`. Publisher first, `mirror` second; reversed gives a white
box (stale ring, no publisher). No audio (see §5).

---

## 10. Rules carried forward

- **Read the source before theorising.** On 2026-08-13, `seat_router.c` and
  `hydrad.cpp` each answered in one read what several rounds of inference got
  wrong. Same lesson as the gfx crash: eleven guesses, one source read.
- **Cheap checks before expensive ones.** Error 87 was a grep.
- **Recovery stick in the bag, tested, before any driver install.** Not after.
- **Never `bcdedit /set bootstatuspolicy ignoreallfailures`** on a machine you
  might need WinRE on — it suppresses the automatic failover *into* WinRE and
  removes your way back in. Combined with `safeboot minimal` it leaves no
  console route to recovery at all; only external media.
- **In WinRE, `HKLM` is WinPE's own RAM hive.** `reg add`/`reg delete` there do
  nothing to the installed system. `reg load HKLM\OFFSYS D:\Windows\System32\config\SYSTEM`
  first, and check `Select\Current` for whether the live set is 001 or 002.
  Drive letters are reassigned on every WinRE boot.
- **Capture `setupapi.dev.log`, `CBS.log`, `System.evtx` before any reset.**
  They are not in `Windows.old`.
- **Machine-specific values belong in this file, not only in `seats.toml`** —
  `seats.toml` appears to be gitignored, so its GUIDs and hardware IDs do not
  reach the repo.
