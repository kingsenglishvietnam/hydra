# ROADMAP.md — where Hydra goes from here

Written 2026-08-17, after mode 6 came up and mode 4 was stopped.

State of play: **modes 1, 2, 3 and 6 all work.** Mode 6 is the one to teach on —
one command via `hydra6.ps1`, and it removed the client-visibility failure that
had been interrupting lessons. Mode 4 is blocked and mode 5 is redundant.

This is what is worth doing next, honestly ranked.

---

## 1. MODE 7 — the one that might make mode 6 obsolete

**Do this first. It is an afternoon and it could delete most of the machinery.**

### The idea

Mode 6 puts the client fullscreen on a virtual display, captures the seat's
desktop with DDA, pushes it through a shared ring, and draws it on the physical
panel with `mirror`. Four stages.

But ask why `mirror` exists at all. From the git history: `mstsc` suppresses
output when covered or minimised, so the panel froze. Modes 2–6 are all
increasingly elaborate answers to that single problem.

**`-suppress-output` solves it directly.** It was added 08-16 and it stops the
client suppressing frames regardless of visibility.

So: run `sdl-freerdp` **fullscreen on the physical panel itself**, with
`display_mode = "off"`. No virtual display. No `session_capture`. No ring. No
`mirror`. The client *is* the seat's screen.

```
sdl-freerdp  --fullscreen on \\.\DISPLAY2-->  the student's monitor
```

One stage instead of four.

### Why this is not obviously wrong

`hydra-no-overlay-needed\seats.toml` — a snapshot from the project's own history
— documents exactly this arrangement and calls it *"the configuration that
actually works"*:

> `"off"` — no mirror, no capture. The panel belongs entirely to mstsc. Use this
> with the .rdp set to FULLSCREEN on the seat's monitor: you get teacher's real
> desktop edge to edge WITH a cursor, and Hydra still does input isolation and
> audio routing.

It was abandoned because mstsc froze when covered — but a fullscreen client on a
monitor with nothing else on it is never covered, and `-suppress-output` closes
the remaining case.

### What is unaffected

Input isolation and audio are **independent of the display path**. `seat_router`
→ `agent:B` injects the wireless pair into the session regardless. `abcap` →
ring → `abren` carries audio regardless. Neither touches `mirror`.

### What might break it

- **Cursor.** With no DDA compositing, the client draws its own pointer. FreeRDP
  does this natively and well, so this is probably an improvement — mode 3's
  blink came from *our* compositing, not from RDP's.
- **Console control.** Your cursor would enter seat B by walking onto DISPLAY2,
  the same gesture as walking onto the virtual display today. No worse.
- **Client focus.** A fullscreen client on a second monitor may grab input on
  focus. If it does, `-suppress-output` plus leaving it unfocused should be
  enough, but this is the thing to watch.

### The test — 15 minutes

```powershell
.\hydra6.ps1 -Stop
(Get-Content C:\Programs\hydra\seats.toml -Raw) -replace '(?m)^display_mode = ".*"', 'display_mode = "off"' | Set-Content C:\Programs\hydra\seats.toml -NoNewline
.\setup.ps1
Start-Service Hydra; Start-Sleep 5
```

```powershell
.\dist\freerdp\sdl-freerdp.exe /list:monitor
```

Take the **2770** index, not the VDD one:

```powershell
.\dist\freerdp\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /d: /cert:ignore /sound -suppress-output /scale:140 +auto-reconnect /f /monitors:<2770 index>
```

Log in. No mirror, no capture agent. Then:

- Is the seat's desktop on the panel, edge to edge?
- Is the cursor visible and smooth?
- Does the wireless pair still drive it?
- Is there sound from the monitor?
- Cover things on the console, switch virtual desktops, wait a minute — does the
  panel keep updating?

**If all five pass, mode 7 replaces mode 6** and you delete `session_capture`,
the ring and `mirror` from the daily path. The virtual display stays useful for
a third seat, but is not needed for two.

If the panel freezes when you switch away, `-suppress-output` did not cover this
case and mode 6 remains correct. Either answer is worth 15 minutes.

---

## 2. MODE 6 ENHANCEMENTS — small, real

Assuming mode 7 does not supersede it.

### Start at logon

`hydra6.ps1` is one command but still a command. A scheduled task at logon, run
as your account with highest privileges, makes the seat come up with the
machine. The virtual display is already permanent via `devcon`, so nothing else
is needed.

The caveat: the client needs `teacher`'s password typed. Either store it the way
`HydraProto` does (listener registry key) or accept one prompt per boot.

### Third seat

`vdd_settings.xml` → `<count>2</count>` gives a second virtual display. Each seat
gets its own parked client, its own `seats.toml` entry, its own physical panel.
No driver work.

Watch CPU before promising it to anyone — each seat is another `sdl-freerdp` and
another capture path.

### Mode 3's cursor blink

Measured and root-caused on 08-16 but not fixed: frames publish at ~124/sec while
`agent:B` publishes positions at ~61/sec, and the pointer is composited from
`curX/curY` on every publish. Roughly every other frame re-stamps the cursor at
the previous coordinates.

Fix is either damping the publish rate to the cursor rate, or skipping the
composite when `curSeq` has not advanced — but note the second was analysed and
rejected once already, because the composite draws into the ring on every
publish and skipping would drop the cursor entirely.

Only matters if mode 3 stays in use. Mode 7 would retire it.

### PROBLEM 5 — the reboot tax

`hydrardp` dying leaves RDP-Wrapper holding a session that only a reboot clears.
~40 lines: a supervisor that runs `logoff <id>` on *any* exit — clean, crashed or
killed. `SetUnhandledExceptionFilter` covers crashes; `TerminateProcess` cannot
be intercepted from inside, so it needs an external watcher.

`hydra6.ps1 -Stop` already handles the normal case. This is for the abnormal one.

### Publish rate

The throttle comment claims ~60/s but 124 fps was measured. Two paths reach
`hydra_end_paint` — `EndPaint` and the gfx `EndFrame` callback — and the 16ms
gate may only apply to one. More frames than the panel can show is wasted work,
not a fault. Low priority.

---

## 3. MODE 4 — research, not development

Mode 4 gives seat B's *session* its own display, removing the client, capture,
ring and mirror from the pixel path. Real, but now marginal: mode 6 already
removed the failure that mattered, and mode 7 may remove the machinery entirely.

**Status:** `0xD000000D`, UMDF refusing every IddCx driver at load level 0. About
fifteen causes eliminated with evidence. Microsoft's own sample loaded on this
machine on the morning of 08-17 and would not by that evening, with no change to
it — so the environment moved and the differential became unreadable.

Do not resume without a clean baseline.

### Paths, best first

**The RDP IDD angle — unexplored and the most promising.**
`SWD\REMOTEDISPLAYENUM\RDPIDD_INDIRECTDISPLAY` loads fine here. Five
`rdpidd.inf` instances sit in the Display class. Microsoft's own remote-session
IDD works on this machine *today*. The question nobody has asked: can an
ordinary RDP session's display be captured directly, rather than through the
client's framebuffer? That would be mode 4's benefit with no custom driver at
all.

**Ask VDD's maintainers.** They ship a working IddCx driver and have a
Discussions board. *"Has anyone made this a remote-session driver?"* is two
lines to people who have already solved the loading problem. Costs nothing.

**OSR Online, NTDEV.** Where display driver developers actually answer. The
question is now unusually well-formed: same signed binary loads on one devnode
and not another, `0x1f / 0xc0000001`, class key correct, `ConfigFlags 0`, fifteen
causes eliminated.

**A VM for the baseline.** A mode 4 that only works in a VM is worthless — but
the VM is a *measurement*, not a product. It answers "is it this machine or the
method?", which decides whether a reinstall is worth an hour. Nothing else can
answer that.

**WinDbg on `WUDFHost`.** `HostProcessDbgBreakOnStart` under
`HKLM\SYSTEM\CurrentControlSet\Control\WUDF`, attach a *user-mode* debugger. The
definitive tool, never used, and it needs a clean machine first.

**A KMDF display miniport.** Sidesteps the class extension entirely — no
`UmdfExtensions`, no `IddCx0102`, no `WUDFHost`. `0xD000000D` cannot follow you
there. Considerably more work; you would be writing a real display driver.

### And the honest one

Windows MultiPoint Server did this natively and Microsoft retired it. There may
be no supported path because Microsoft removed it deliberately.

---

## 4. MODE 5 — drop it

Its goal was a console-side IddCx driver. VDD is one, working, permanent. The
only thing `iddseat.cpp` would add is per-seat EDID and mode control, and
`vdd_settings.xml` covers resolutions.

`iddseat.cpp` is now well-characterised as broken-in-some-way-we-cannot-see, and
fixing it buys nothing. Leave it in the repo as history.

---

## 5. THE BIGGER QUESTION — Linux

Worth stating plainly because it keeps being the right answer.

`systemd-logind` does multiseat natively. `loginctl attach seat1 <sysfs-path>`,
one seat per GPU output, input devices assigned by udev. No RDP loopback, no
capture pipeline, no shared ring, no driver signing, no class extensions, no
`0xD000000D`.

Everything Hydra fights exists because **Windows makes this hard**, not because
the design is wrong. Mode 4 is an attempt to make Windows behave the way Linux
already does.

If the goal is two good seats rather than making Windows do it, that is the
better engineering — and it is a config exercise, not a driver project.

Hydra remains valuable as the Windows fallback: the students' software, the
teaching materials, and the classroom's expectations are all Windows-shaped.

---

## 6. RECOMMENDED ORDER

1. **Test mode 7** (§1). Fifteen minutes, and it might delete three stages of
   machinery.
2. **Whichever of 6 or 7 wins, make it start at logon.** That is the last
   friction in daily use.
3. **Post the mode 4 question** to VDD Discussions or OSR. Costs nothing while
   you do other things.
4. **Build the Linux seat** in a quiet week, as the real answer.
5. **Leave mode 4** in `MODE4-STEP2.md` for whoever wants it — you, or someone
   else, or a future assistant who reads the repo before theorising.

Do not resume mode 4 on this machine without a clean baseline. That is the one
thing these sessions proved conclusively.
