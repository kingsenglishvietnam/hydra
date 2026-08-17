# MODE6-START.md — cold start, every time

Mode 6: the RDP client runs fullscreen on a **virtual display**, so it can never
be covered, minimised, or left on an inactive virtual desktop — which is the
whole family of failures that shaped modes 1–3. `mirror` draws seat B on the
physical panel for the student; a second windowed `mirror` on virtual desktop 2
is how you drive the seat yourself.

Run everything from the elevated Hydra Shell at `C:\Programs\hydra`.

---

## 1. Virtual display — recreate it if it vanished

`devgen` devices do **not** reliably survive a reboot. Check first:

```powershell
cd C:\Programs\hydra; .\dist\clip_console.exe
```

Three monitors expected. If only two, recreate:

```powershell
& 'C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe' /add /hardwareid "Root\MttVDD"
Start-Sleep 8
Get-PnpDevice | Where-Object FriendlyName -match 'Virtual Display' | Select-Object Status, InstanceId
.\dist\clip_console.exe
```

**GATE: Status OK.** A stale `Unknown` entry alongside it is a phantom from the
previous boot — harmless, it clears itself.

Only ever run `devgen /add` **once**. It does not check for an existing device,
so every extra run adds another virtual monitor. If you end up with two:

```powershell
$d='C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe'
Get-PnpDevice | Where-Object FriendlyName -match 'Virtual Display' | Select-Object Status, InstanceId
& $d /remove '<the extra InstanceId>'
```

### If the driver package itself is gone

```powershell
((pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n" | Where-Object { $_ -match 'mttvdd' }).Count
```

Zero means it needs reinstalling:

```powershell
pnputil /add-driver C:\Programs\vdd\VirtualDisplayDriver\MttVDD.inf /install
```

**Use the shipped package**, signed by SignPath Foundation — not
`dist\vdd\MttVDD.inf`, the from-source build, which does not load.

### If it comes up at 800x600

Settings → System → Display → select **VDD by MTT** → Resolution **1920x1080**,
Scale **100%**, and drag it above the console panel. Windows usually remembers,
but the position resets when devices are recreated.

Target geometry:

```
monitor 0: (0,0)-(3240,2160)        \\.\DISPLAY1   Surface panel, console
monitor 1: (3240,1070)-(5160,2150)  \\.\DISPLAY2   2770, seat B's panel
monitor 2: (0,-1080)-(1920,0)       \\.\DISPLAY4   VDD, where the client lives
```

The VDD monitor's bottom edge touching the console panel's top edge is what
lets your cursor walk into seat B. Narrow that overlap if you keep entering it
by accident — drag it sideways so only a corner touches.

---

## 2. Service first

It creates the shared sections and the router's listener. **A `mirror` started
before the service is up gets refused and stays refused** — that is what made
input forwarding look broken.

```powershell
Start-Service Hydra; Start-Sleep 5; .\dist\hydractl.exe status
```

Expect `router`, `abren:B`, and — once a seat session exists — `abcap:B`,
`agent:B`, `capture:B`. `clip` only appears if `confine_monitor` is set in
`seats.toml`.

---

## 3. Client, fullscreen on the virtual display

**Read the monitor index every time.** It shifts between boots — it has been 3,
4 and 5 within a day.

```powershell
.\dist\freerdp\sdl-freerdp.exe /list:monitor
```

Take the number beside **[VDD by MTT]**, then:

```powershell
.\dist\freerdp\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /d: /cert:ignore /sound -suppress-output /scale:140 +auto-reconnect /f /monitors:3
```

Log in as `teacher`. Echo is off, so a typo shows as
`ERRCONNECT_LOGON_FAILURE`.

What each flag is for, since removing one silently breaks something:

| flag | why |
|---|---|
| `/sound` | **REQUIRED.** Creates the seat's audio ENDPOINT. Without it `abcap` fails `0x80070490` forever and there is no sound, even though `audio_bridge` carries the audio, not this channel. |
| `-suppress-output` | Stops the client suppressing frames when it believes nothing is visible. |
| `/scale:140` | Session DPI, so the seat's UI is readable while the display stays 1:1. |
| `/d:` | Puts login focus on the password field, not the domain field. |
| `/f /monitors:N` | Fullscreen on the virtual display. The point of mode 6. |

---

## 4. Panel mirror — what the student sees

```powershell
.\dist\mirror.exe B \\.\DISPLAY2
```

Fullscreen on the physical monitor. **One-way — it forwards no input.**

Launch it from whichever virtual desktop you are on; fullscreen placement is by
monitor, not by desktop.

Wait until the client is logged in and drawing before starting it. Against an
empty ring it sits blank.

---

## 5. Control mirror — how you drive the seat

```powershell
.\dist\mirror.exe B --window 1600x900 56789
```

**The port argument is required** and it is **56789**, the router's agent port.
Without it the window renders but forwards nothing.

Then `Win+Ctrl+Right` to move yourself to virtual desktop 2 with it.

Hover the window and your console mouse and keyboard drive seat B. Move off it
and they come back to the console.

Two things to know:

- It forwards whenever your pointer is **over** it, not only when focused. Park
  it somewhere you will not cross by accident.
- Close it when you are not actively driving the seat, or leave it on VD2.

The student's wireless keyboard and mouse work independently of all this —
`seat_router` captures them and `agent:B` injects them straight into the
session. **No window, no focus, no virtual desktop involved.** They keep working
while you are elsewhere.

---

## 6. Verify

```powershell
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

Move something in seat B while this runs — DDA publishes nothing on a static
desktop, and a still picture is not a failure.

| field | healthy |
|---|---|
| `frame` | climbing, ~25–120/sec with activity |
| `curSeq` | ~60/sec |
| `ready` | `1` — `0` means nothing populated meta and `mirror` shows a white box |
| `STALLED` | absent — if present, `capture:B` is attached but `EnumOutputs` is empty |

**The mode 6 test:** switch to another virtual desktop, cover things, wait a
minute, then run it again. `frame` still climbing is the proof — the client is
on a display nothing ever touches.

---

## 7. Shutdown

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay -EA SilentlyContinue | Stop-Process -Force
Stop-Service Hydra
query session
```

Then `logoff <teacher session id>`. Leaving the session disconnected means the
wrapper holds it, and only a reboot clears that.

The virtual display can stay — it costs nothing and saves recreating it.

---

## 8. When something is wrong

| symptom | cause |
|---|---|
| only two monitors | `devgen` device gone. §1. |
| virtual display at 800x600 | resolution reset when the device was recreated. §1. |
| client lands on the wrong screen | monitor index shifted. Re-read `/list:monitor`. |
| no sound | `/sound` missing. Check `abcap:B` is `running`, not `waiting`. |
| windowed mirror shows the seat but forwards nothing | port missing, or it was started before the service. Restart the service, then the mirror. |
| panel blank or white | started before the client was publishing. Kill it and relaunch. |
| `frame` frozen | seat B's screen is static. Move something before concluding anything. |
| `ERRCONNECT_ACTIVATION_TIMEOUT` | `sc qc TermService` must read `TYPE : 10 WIN32_OWN_PROCESS`. A reset undoes it. Otherwise the stack is wedged — reboot. |
| seat cursor visible but nothing responds | `agent:B` err 5. `Get-Content C:\ProgramData\Hydra\logs\agent_B.log -Tail 5`. Fires when seat B is at a lock screen. |

**Escape hatch, any time:** type `Stop-Service Hydra` and press Enter, blind if
necessary. Releases input in ~2s. The service is Manual-start, so a reboot also
clears it.

---

## 9. Do not

- **Run `mirror B \\.\DISPLAY2` fullscreen while testing at the console** unless
  you mean it. It is borderless with no exit path and has trapped the console
  twice. Use `--window` for anything exploratory.
- **Install or update Logitech software.** Installing that driver *inside the
  seat session* is the confirmed PROBLEM 1 trigger — it tore down the session's
  display stack and took 1806 capture retries to give up. The Download Assistant
  has been removed from the Run key; leave it removed.
- **Stage `dist\vdd\MttVDD.inf`.** That is the from-source build and it does not
  load. The shipped package in `C:\Programs\vdd\VirtualDisplayDriver\` is the
  working one.
- **Run `devgen /add` more than once** per boot.
- **Judge any driver result before a demanded reboot.** `CM_PROB_NEED_RESTART`
  makes everything downstream unreadable, and it produced two false conclusions
  in one afternoon.
