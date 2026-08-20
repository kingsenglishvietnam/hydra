# FUTURE.md — refining what works, and what might come next

Written 2026-08-20, the day mode 4 was closed by decision rather than blocker.

**Where things stand.** Modes 1, 2, 3, 5, 6 and 7 all work. **Mode 7 is what you
teach on** — `sdl-freerdp` fullscreen on the seat's own panel, one command via
`hydra7.ps1`, no capture, no ring, no mirror. Mode 4 is closed: the driver
works, but `GetInputHandles` needs kernel input miniports, and the only thing it
would buy is removing one user-mode process that modes 3 and 7 already handle.

---

## PART 1 — REFINING WHAT WORKS

Ordered by value per hour, not by interest.

### 1.1 Autostart at logon — the last daily friction

`hydra7.ps1` is one command, but it is still a command every morning.

A scheduled task at logon, running as your account with highest privileges,
brings the seat up with the machine. The obstacle is `teacher`'s password.

Three options, in order of preference:

- **Accept one prompt per boot.** Simplest, and honestly fine.
- **Store it the way `rdsprov-register.ps1` does** — a registry value the
  launcher reads. Same exposure as what already exists for HydraProto.
- **Never use `/p:` on the FreeRDP command line.** The password lands in the
  process command line where any user on the machine can read it.

Worth doing when the daily command starts to grate, not before.

### 1.2 Video playback under real lesson conditions

**Still untested, and it is the one unknown that could affect a lesson.**

RDP does software rendering in the seat session. A full-screen YouTube lesson
may hold up fine or may not. Ten minutes with a real video answers it.

If it struggles, the knobs in order:

- `/gfx:RFX` on the client — enables the RemoteFX codec path
- `/network:lan` — changes the bandwidth heuristics
- Lower `/scale:` — fewer pixels to push
- **Never `/gfx:h264` or `HYDRA_GFX=1`** — this FreeRDP build is
  `WITH_VAAPI_H264_ENCODING=ON` and crashes

If nothing helps, that is a real finding: video-heavy lessons run on the console
seat and seat B does everything else.

### 1.3 Make the launchers survive PowerShell updates

`hydra-start.ps1` broke on PS 7.4's
`$PSNativeCommandUseErrorActionPreference` — native stderr became terminating,
and `hydractl`'s "not reachable" progress message aborted the script. Fixed in
`hydra-view.ps1`, not in `hydra-start.ps1`.

```powershell
Select-String -Path C:\Programs\hydra\*.ps1 -Pattern '2>&1' | Select-Object Path, LineNumber
```

Every one of those is a latent version-dependent failure. Bracket each with
`$ErrorActionPreference = 'Continue'` or use `6>&1` where the target writes via
`Write-Host`.

The Hydra Shell shortcut also broke when PowerShell updated — it pointed at a
versioned WindowsApps path. `install-shortcut.ps1` regenerates it; worth making
it target the stable `pwsh.exe` alias instead.

### 1.4 Mode 3's cursor blink

Measured and understood, not fixed: frames publish at ~124/sec while `agent:B`
publishes positions at ~61/sec, and the pointer is composited on every publish —
so roughly every other frame re-stamps it at the previous coordinates.

Fix is damping the publish rate to the cursor rate. Note that skipping the
composite when `curSeq` has not advanced was analysed and **rejected** — the
composite draws into the ring on every publish, so skipping drops the cursor
entirely.

Only matters if mode 3 stays in use. Mode 7 retired it from daily work.

### 1.5 PROBLEM 5 — the reboot tax

A client dying leaves RDP-Wrapper holding a session that only a reboot clears.
`hydra7.ps1 -Stop` handles the normal case; this is for crashes.

~40 lines: an external watcher that runs `logoff <id>` on *any* client exit.
`SetUnhandledExceptionFilter` covers crashes from inside, but `TerminateProcess`
cannot be intercepted, so it needs to be external.

### 1.6 Housekeeping that bit hard this week

- **Driver store discipline.** Eleven stale `iddseat` packages accumulated in one
  afternoon and package `oemNN` numbers get **recycled**, so a stale class key
  can reference a number that now belongs to a different driver. Two packages is
  correct.
- **`safety-gate.ps1` before every driver install.** It was skipped on 08-20 and
  the machine came up with a blank screen. Recovered by unplugging the USB hub,
  but that was luck.
- **Phantom Enum devnodes need SYSTEM.** `Remove-Item` reports success and does
  nothing. `PsExec64 -s reg delete` works. They do not appear to block new
  devnodes, so this is tidiness rather than necessity.

---

## PART 2 — NEW MODES WORTH CONSIDERING

### Mode 8 — a third seat

Mode 7 does not scale: each seat needs its client somewhere always-composited,
and there is only one physical panel to give. **Mode 6 is the shape for seat C.**

- `vdd_settings.xml` → `<count>2</count>` gives a second virtual display
- seat B stays on mode 7, its own panel
- seat C runs mode 6: client parked on a virtual display, `mirror` on its panel
- another `seats.toml` entry, another wireless pair bound by hardware ID

The virtual display is already permanent at `ROOT\DISPLAY\0000`.

**Watch CPU before promising it to anyone.** Each seat is another
`sdl-freerdp`; seat C adds a capture path and a mirror on top.

### Mode 9 — mode 5's virtual display as a second physical output

This is a genuinely new idea and it is cheap, because **mode 5 already works**.

Your own IddCx driver presents a real virtual monitor on the console. So does
VDD. Neither is doing anything useful yet — mode 5 was pursued as a stepping
stone to mode 4, and mode 4 is closed.

But a virtual display is a real display. Two possible uses:

- **A staging screen.** Park the client for a seat that has no physical panel
  yet, or hold lesson material off-screen and mirror it to the panel on demand.
- **Per-seat EDID control.** `iddseat.cpp` reads seat properties and builds its
  EDID from them — the thing VDD cannot do. If a seat needs a specific
  resolution or refresh the panel does not advertise, your driver can lie
  convincingly where a real monitor cannot.

Neither is a need today. Worth remembering the capability exists.

### Mode 10 — no RDP at all, via a second GPU

The honest ceiling on Windows. Not reachable on this hardware, but worth writing
down so it is not rediscovered.

A VM per seat with GPU passthrough gives true multiseat: separate sessions,
hardware acceleration, no RDP, no capture. Requires a discrete GPU that can be
passed through, and the Surface Book 3's muxless 1660 Ti cannot be. Viable on a
desktop with two GPUs.

If the classroom machine is ever replaced, this changes the calculus entirely.

### Mode 11 — Linux multiseat

`systemd-logind` does this natively. `loginctl attach seat1 <sysfs-path>`, one
seat per GPU output, input assigned by udev. No RDP loopback, no capture
pipeline, no shared ring, no driver signing, no class extensions.

Everything Hydra fights exists because **Windows makes this hard**, not because
the design is wrong.

Hydra stays valuable as the Windows answer — the students' software, the
teaching materials and the classroom's expectations are all Windows-shaped. But
if the goal is two good seats rather than making Windows do it, this is the
better engineering and it is a config exercise, not a driver project.

---

## PART 3 — THINGS THAT WOULD MAKE ANY OF IT EASIER

### A second machine for driver work

The one recommendation from the whole week that would have saved the most time.
Two incidents — the 08-12 boot loop and the 08-20 blank screen — both happened
on the machine you teach from.

A second-hand ThinkPad means: install, break, do not care. And the differentials
stay valid because nothing else is churning the driver store.

### Instrumentation before theory

The method that broke five days of deadlock, worth stating as a rule:

**Every hypothesis I generated by reasoning failed. Every fix came from reading
a file already on disk, or from a log line that did not exist yet.**

- `0xD000000D` was solved by `OutputDebugStringA` proving `DriverEntry` ran
- The three IddCx bugs came from diffing against the WDK sample and `MttVDD.inf`
- The DXGI bypass was closed by testing it rather than reasoning about it

When something is opaque, add logging first and read a working reference second.
Theorise last, if at all.

### Read the repo and `git log` before answering

Commit messages run ahead of every `.md` here. Nearly every wrong turn this week
was a guess made before looking at a file that already had the answer —
including `/sound` being documented at `hydra-start.ps1:160`, and the mode 1
arrangement documented in a snapshot folder's `seats.toml` comments.

---

## RECOMMENDED ORDER

1. **Use mode 7 for the term.** Let it prove itself in real lessons before
   changing anything.
2. **Test video playback** (1.2). Ten minutes, and it is the only unknown that
   could affect a lesson.
3. **Fix the `2>&1` patterns** (1.3). They are latent breakage on the next
   PowerShell update.
4. **Autostart** (1.1) when the daily command starts to grate.
5. **Seat C via mode 6** (Mode 8) if and when a third student needs one.
6. **Linux multiseat** (Mode 11) in a quiet week, as the real answer.

Mode 4 stays closed unless someone answers the NTDEV question and says the input
handles can be stubbed without kernel drivers. `COMMUNITY-QUESTIONS-3.md` is
written and ready if that day comes.
