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

**Windows resets that assignment on every reboot**, and accumulates a new registry entry each time you change it without removing the old ones — so a stale entry pointing at a removed device can win, and the UI still looks correct while nothing plays. `audio-pin.ps1` captures the working state and `hydra-start.ps1` restores it before mstsc launches (the timing matters: a per-app change doesn't take on a stream that's already open).

### The first-app-gets-silence problem

The seat's Remote Audio endpoint goes bad when it has been idle. **The first application to open it hears nothing.** Open a second app and *that* works — and then the first one starts working too. Browsers lose this race reliably; media players often win it, which disguised it as a Chrome bug for hours.

`chrome://media-internals` settled where the audio was going: Chrome reported `kPlaying`, `BUFFERING_HAVE_ENOUGH`, a cleanly selected decoder and no errors. It was decoding and handing off PCM the whole time. Nothing was wrong above the render path.

**The fix is to make something harmless the first opener.** `audio_prime` in `seats.toml`:

| Value | What it does |
|---|---|
| `"chime"` (default) | Plays one short sound in the seat's session at startup. No extra process. **Verified working.** |
| `"keepalive"` | Runs `audio_keepalive.exe` for the whole session, holding a silent stream open so the endpoint can never go idle. Use if the chime wears off mid-lesson. |
| `"off"` | Nothing. |

Mid-lesson recovery, if audio ever goes quiet:
```
.\dist\hydractl.exe chime B
```
`install-audiofix.ps1` puts that on the seat user's desktop as **Fix Audio** — no elevation needed, since hydractl only messages the service.

> **Do not try restarting Audiosrv.** It was tested from four contexts — the console session, a SYSTEM scheduled task in session 0, a SYSTEM token inside the seat's session, and the console admin's *elevated* token inside the seat's session (the exact combination that appears to work when typed by hand). **None of them fix it.** A restart leaves the endpoint idle, which just hands the problem to whoever opens it next; the manual successes had another app already holding the endpoint open. `hydractl audiofix <seat>` still exists but should not be needed.

```
.\audio-pin.ps1 -Save      # once, while the audio is working
.\audio-pin.ps1 -Show      # compare saved vs live
```

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
audioqualitymode:i:0
smart sizing:i:1
username:s:teacher
```

- `screen mode id:i:1` — windowed. Fullscreen puts mstsc on the panel, covering mirror.
- `audiomode:i:0` — the lag-free mode, and the one the audio design depends on.
- `smart sizing:i:1` — scales the whole desktop to the window instead of cropping.
- `audioqualitymode:i:0` — dynamic. Fixes the first-app-has-no-sound race; `:2` (uncompressed) exhibited it.

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
| First app opened in the seat's session has no sound; a second app works, then the first works too | The endpoint went idle. `hydra-start.ps1` primes it automatically; to fix mid-lesson run `.\dist\hydractl.exe chime B` or click **Fix Audio** on the seat desktop. If it recurs within a session, set `audio_prime = "keepalive"` in `seats.toml`. **Do not restart Audiosrv** — see the audio section for the four contexts that were tried and don't work. |
| RDP window goes fullscreen and grabs input | A window sized at or beyond the screen tips mstsc into fullscreen. `minify-mstsc.ps1` now clamps every size to the work area minus a margin. Escape with **Ctrl+Alt+Break**. |
| Teacher's session killed / `ConnQ` stuck in `query session` | Killing mstsc takes the session with it and can wedge the RDP stack. `reset session N`, then `Restart-Service TermService -Force`; if it survives both, reboot. Don't kill mstsc. |
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

**RDP-Wrapper / TermWrap**
- A Windows update replacing `termsrv.dll` breaks concurrent sessions: the listener still answers (`127.0.0.2 Established`) while session creation fails (`ConnQ`, no session). Happened on KB5120102. Fix is updating the wrapper, not anything in Hydra.
- This machine runs **llccd/TermWrap** (`TermWrap.dll` + `Zydis.dll`), which finds its patch points by disassembly rather than a version `.ini`. Update: stop `TermService`, replace the DLLs, merge `Install_termwrap_only.reg`, reboot.
- Use **`Install_termwrap_only.reg`** — `UmWrap` and `EndpWrap` are only needed on Server and Home editions. Confirm `AudioEnumeratorDll` under `HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp` reads `rdpendp.dll`, not `EndpWrap.dll`.
- Killing mstsc, or sometimes a clean sign-out, can leave a session wedged: `ConnQ` in `query session` that `reset session` can't clear. Try `Restart-Service TermService -Force`; if the stale `127.0.0.2 Established` connection survives, reboot.

**Platform**
- Windows Server / RDS — no upgrade path from Pro; clean install, CALs, and poor Surface driver support.
- ASTER, BeTwin, SoftXpand — ASTER is the only surviving commercial option; the rest are discontinued. MouseMux and similar are shared-desktop multi-cursor tools, not multiseat.

---

# LOCKING DOWN TEACHER

Hide shutdown/restart, leaving Sign out — run **inside teacher's session**:

```
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer" /v NoClose /t REG_DWORD /d 1 /f
```
```
taskkill /f /im explorer.exe & start explorer.exe
```

`NoClose` only hides UI, and Windows 11 doesn't always honour it. To actually remove the capability — including `shutdown /s` from a terminal — use `secpol.msc` → Local Policies → User Rights Assignment → **Shut down the system**, and leave only Administrators.

UAC prompts render slowly and freeze the panel, because the secure desktop can't be duplicated. To make them prompt on the normal desktop instead:

```
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v PromptOnSecureDesktop /t REG_DWORD /d 0 /f
```

That is a real reduction in UAC's protection against spoofed prompts — a deliberate trade for a classroom machine, not a free win.

---

# STILL OPEN

- **A full cold boot end to end.** Every piece has been verified, but not in one run from power-on since the TermWrap update.
- **A third seat.** `seats.toml` takes multiple `[[seat]]` blocks and `plan_procs` loops over them, so this is validation rather than new code — needs a monitor, an HDMI cable, a device pair and a port. Untested at >2 seats.
- Seat B logging the seat user in from the lock screen — the SYSTEM-token agent should allow it; untested.
- Whether `audio_prime = "chime"` holds for a whole lesson, or the endpoint goes idle again after a long silence. If it does, switch to `"keepalive"`.
- `teacher.rdp` is not in this package — it holds saved credentials. Keep your own copy in `C:\Programs\hydra`.

---

# BEYOND ASTER

Feature parity is close. What Aster does that this doesn't: per-seat USB (webcams, headsets, drives), native display latency (this costs a GPU→CPU→GPU round trip, ~8 MB/frame at 1080p), and coming up at boot without seat 1 logging in first.

The more interesting direction is that this is a *teaching* machine and Aster doesn't know that. The pieces already exist — cross-session input injection, per-seat framebuffers in shared memory, a supervisor with a control channel:

- **Broadcast** — push the teacher's frame to every seat's panel. The transport is already there.
- **Lock a seat** — freeze input to one seat from `hydractl`. Trivial with `agent:<seat>` in place.
- **Watch a seat** — teacher views a student's panel. Same transport, reversed.

That's classroom-management software, a category above what Aster does, and mostly plumbing that already works.
