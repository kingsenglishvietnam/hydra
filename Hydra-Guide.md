# Hydra — Rebuild & Operations

**Rev 2026-08-03.** Supersedes every earlier guide. Where something was tested and rejected, it says so — the dead ends cost days, and re-walking them is the main risk.

---

# WHAT THIS IS

One Windows PC, two seats. Each seat has its own keyboard, mouse, screen and audio output, simultaneously and independently.

| | Seat 1 (you) | Seat 2 (teacher) |
|---|---|---|
| Session | console (1) | RDP (2) |
| Input | wired kbd/mouse | wireless kbd/mouse, matched by hardware ID |
| Display | laptop screen | external monitor via `\\.\DISPLAY2` |
| Audio | Realtek `548a2a1a` | Intel Display Audio `623f2512` |

## How the display actually works

```
teacher's session
   session_capture   Desktop Duplication + cursor composited into the frame
        |            (runs as the user, in session 2)
        v
   Global\HydraSeat_B_pix     shared memory, seqlock
        |
        v
   mirror            uploads, scales, presents fullscreen on the panel
                     (runs INTERACTIVELY in session 1 — not a service child)
```

Two constraints that took a long time to find, both non-negotiable:

- **D3D11 shared textures do not cross a terminal-services session boundary.** `OpenSharedResourceByName` resolves the name then fails `E_INVALIDARG` (0x80070057) whatever access flags you pass. Frames travel as *pixels in shared memory* instead. Costs a readback and an upload per frame; works.
- **DDA excludes the cursor.** The pointer shape and position arrive as separate metadata and must be composited in. Doing that is why the cursor survives the Start menu, which the old `cursor_overlay` never could — an overlay is a window, and windows lose z-order to shell surfaces.

## How the audio actually works

On `audiomode:i:0` teacher's session has **no real audio hardware** — it ships audio down the RDP channel and **`mstsc.exe`, running in the console session, plays it**. So the isolation is a single per-app output assignment:

- System default output → **Speakers (Realtek)** — seat 1
- Volume mixer → `mstsc.exe` → **2770 (Intel Display Audio)** — seat 2

No cable, no loopback agent, no extra process. Per-app assignment doesn't disturb the shared default, which is exactly what made `audiomode:i:1` fail.

---

# PART 1 — WIPE

Only for a genuine clean slate. To just update binaries, skip to Part 2.

**Stop the service AND mirror** — mirror isn't a service child, so `Stop-Service` doesn't touch it, and the linker will fail `LNK1104` if it's holding the exe.

```
Get-Process mirror -ErrorAction SilentlyContinue | Stop-Process -Force
```
```
Stop-Service Hydra
```
```
cd C:\Programs\hydra
```
```
.\dist\hydrad.exe uninstall
```

Remove any leftover IDD driver packages (the virtual display is retired):

```
pnputil /enum-drivers | Select-String "iddseat" -Context 3,0
```
```
pnputil /delete-driver oemNN.inf /uninstall
```

Delete the files:

```
Remove-Item C:\Programs\hydra -Recurse -Force
```
```
Remove-Item C:\ProgramData\Hydra -Recurse -Force
```
```
Remove-Item "C:\Program Files\Hydra" -Recurse -Force
```

**Leave `C:\Programs\Interception` alone** — separate kernel driver, still required.

---

# PART 2 — BUILD

```
Expand-Archive -Path "$HOME\Downloads\hydra.zip" -DestinationPath C:\Programs -Force
```

> Destination is **`C:\Programs`**, not `C:\Programs\hydra`. The zip contains its own `hydra\` folder.

```
cd C:\Programs\hydra
```

Restore the Interception SDK files (never in the zip):

```
copy C:\Programs\Interception\library\interception.h input\
```
```
copy C:\Programs\Interception\library\x64\interception.lib input\
```

Build:

```
.\build.ps1
```

✅ **`Built (12)`**, no `FAILED`. If `seat_router` is SKIPPED, the Interception files didn't land.

**No driver build.** `build-driver.ps1` and `build-overlay.ps1` are not run — the IDD and the cursor overlay are both retired.

---

# PART 3 — CONFIGURE

```
notepad seats.toml
```

```
kbd_id       = "VID_1EA7&PID_0066"
mouse_id     = "VID_046D&PID_C548"
port         = 56789
monitor      = '\\.\DISPLAY2'
session      = "user:teacher"
display_mode = "capture"
# audio_route stays COMMENTED OUT
```

Put **`teacher.rdp`** in `C:\Programs\hydra` (the startup script looks for it there) containing at least:

```
screen mode id:i:1
audiomode:i:0
smart sizing:i:1
username:s:teacher
```

- `screen mode id:i:1` — windowed. Fullscreen puts mstsc on the panel, covering mirror.
- `audiomode:i:0` — the lag-free mode, and the one the audio design depends on.
- `smart sizing:i:1` — scales the whole desktop to the window instead of cropping.

Connect once manually with **Allow me to save credentials** ticked, so startup doesn't stop at a password prompt.

Deploy:

```
.\setup.ps1
```

Re-run this after **every** `seats.toml` edit — the service reads the copy in `dist\`.

---

# PART 4 — DAILY START

One command:

```
.\hydra-start.ps1
```

It sequences: console → virtual desktop 2 · launch `teacher.rdp` · wait for teacher's session · **wait for teacher to be logged in** (watches for `LogonUI.exe` to exit — a locked session looks identical to a live one in `query session`) · start the service · wait for `capture:B` · start mirror, retrying up to 3× · size the RDP window to the laptop screen · pin it to all virtual desktops · print status.

**Order matters and the script enforces it.** mirror only works reliably when started *after* capture is publishing. Started earlier — as a service child, a logon scheduled task, or a Startup shortcut — it ends up stuck at ~2 MB with CPU climbing. All three were tried.

Optional install of VirtualDesktop for the console move:
```
Install-Module VirtualDesktop -Scope CurrentUser -Force
```

---

# PART 5 — VERIFY

**Display.** Panel shows teacher's real desktop. Press the **Windows key on seat B's keyboard** — the Start menu opens on the panel and **the cursor stays visible over it**.

**Input.** Seat B's devices drive teacher only; yours stay yours. Clicking into the mstsc window also lets seat 1's devices drive teacher.

**Audio.** Teacher → monitor, yours → laptop, simultaneously, no bleed.

**Health check at a glance:**
```
Get-Process mirror
```
~70–98 MB, sub-second CPU = presenting. ~2 MB with CPU climbing = stuck; restart it.

---

# TROUBLESHOOTING

| Symptom | Cause / fix |
|---|---|
| Panel frozen | The RDP client stopped being composited. Three routes, all measured: **minimized**, **moved off-screen**, or **sitting on an inactive virtual desktop**. It must be visibly on-screen. `.\minify-mstsc.ps1 -Fill` |
| Panel blank, mirror healthy | Run it in the foreground: `.\dist\mirror.exe B "\\.\DISPLAY2"` — output goes straight to the terminal. This is the diagnostic that has actually worked; hydrad's log capture for mirror is unreliable. |
| mirror ~2 MB, CPU climbing | Started before capture was publishing. Kill it and re-run `.\hydra-start.ps1`. |
| Seat B input dead, `err 5` in `agent_B.log` | Was the lock-screen/UAC case. Now fixed — the agent runs **as SYSTEM in the session** and can attach to any desktop. Confirm with `launched ... as SYSTEM` in `hydrad.log`. |
| Seat B input dead, `kbd=0 mouse=0` in `router.log` | Devices not present — wireless peripherals asleep. Press a key. Not a fault. |
| No audio after a reboot or device change | Volume mixer → `mstsc.exe` → **2770 (Intel Display Audio)**. Reconnect the session afterwards; per-app changes don't take on a stream that's already open. |
| Audio ignores every device you pick | Stale per-app entries, usually after removing an audio device. Settings → Sound → Volume mixer → **Reset**, or clear them in the registry (see below). |
| `LNK1104` on mirror.exe | mirror is running. `Get-Process mirror | Stop-Process -Force` first. |
| `capture:B: waiting` | Teacher's session isn't up or isn't logged in. |

**Per-app audio assignments** accumulate and are never cleaned up — including entries for devices that no longer exist, which resolve to nothing while the UI looks correct:

```
Get-ChildItem 'HKCU:\Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore' | ForEach-Object { "{0} = {1}" -f $_.PSChildName, (Get-ItemProperty $_.PSPath).'(default)' }
```

---

# SETTLED — DO NOT REVISIT

**Audio**
- `audiomode:i:1` — device selection is shared between seats; change one, both move.
- `audiomode:i:2` — audio off.
- Endpoint loopback from inside teacher's session — feedback loop.
- Process loopback from session 0 — activates cleanly, delivers silence.
- VB-CABLE + `route_endpoint` — worked, then proved redundant once mstsc could be pointed straight at the monitor.

**Display**
- D3D11 shared textures across sessions — `E_INVALIDARG`, any flags. Shared memory is the transport.
- Remote-session IDD — `REMOTE_SESSION_DRIVER` can't be software-enumerated (`CM_PROB_REINSTALL`), and RDP-Wrapper never enumerates one. Tested both arms.
- `cursor_overlay` — always loses z-order to the Start menu. Compositing into the frame is the fix.
- mirror as a service child — no interactive token, empty log, nothing on the panel.
- mirror as a scheduled task or Startup shortcut — starts too early, ends up stuck.
- Hiding mstsc — minimizing, off-screen, and inactive virtual desktops all freeze the panel.
- Closing mstsc — destroys the session's display entirely.

**Platform**
- Windows Server / RDS — no upgrade path from Pro; clean install, CALs, and poor Surface driver support.
- ASTER, BeTwin, SoftXpand — ASTER is the only surviving commercial option; the rest are discontinued. MouseMux and similar are shared-desktop multi-cursor tools, not multiseat.

---

# STILL OPEN

- Seat B logging teacher in from the lock screen — the SYSTEM agent should allow it; untested.
- Whether the mstsc audio assignment survives reboots now the registry is clean.
- `smart sizing:i:1` in `teacher.rdp` — added, not yet confirmed.
