# ROADMAP.md

Rewritten 2026-10-11, after mode 7 moved to H.264. **Hydra is in active
development.**

**Mode 7 on H.264 is the one to teach on**: `.\hydra7-avc.ps1`. mstsc runs
full-screen on the seat's own panel; the seat's desktop is drawn on the Iris Plus
and encoded by Quick Sync; the seat's pointer is drawn over the panel from the
console. `.\hydra7.ps1` — FreeRDP and RemoteFX — is the fallback.

Modes 1, 2, 3 and 6 also work and are kept as fallbacks. Mode 4 is closed,
mode 5 is dropped.

---

## What is actually left

Smallest first.

### Check text sharpness under H.264

H.264 is a video codec; RemoteFX on a loopback is close to lossless. Video has
been checked, text hasn't. A dense document, a spreadsheet and a slide, side by
side under both launchers, settles whether 4:4:4 is enough.

### Scope TCP-only to the seat

`fClientDisableUDP = 1` fixed seat B's drops, but it applies to every mstsc
connection from this laptop, including remote desktops over the internet, where
UDP helps. Set it in `hydra7-avc.ps1` and clear it on `-Stop` instead.

### Guard against minimizing

mstsc stops drawing when minimized. Win+D on your keyboard hides seat B until
something restores it. `client-watchdog.ps1` already restores a minimized mstsc;
start it with the seat.

### Put the cursor overlay under `hydrad`

`hydra7-avc.ps1` starts `cursor_overlay_seat.exe` once, in a minimized
PowerShell. `hydrad` supervises agent:B; it could supervise the overlay the same
way — restarted if it dies, and no window of its own.

### Sharper application pointers

Standard pointers come from the seat's own 256 px scheme files. Pointers that
apps draw themselves only exist as the 32 px live image, enlarged. Rendering them
at size would need `DrawIconEx` into the larger image.

### Autostart at logon

`hydra7-avc.ps1` is one command but still a command. A scheduled task at logon —
your account, highest privileges — brings the seat up with the machine. Ticking
**Remember me** in mstsc's password prompt removes the last interaction.

### Mouse handedness

Compensated in two places — `MOUSE_MAP` in `seatB_agent.c` and
`SwapMouseButtons` in the seat's profile. If a mouse ever comes out backwards,
one layer is doing it twice. Remove the Windows setting first; it is per-profile
and easy to lose track of.

### Retire what mode 7 does not use

`session_capture`, the shared ring and `mirror` are not in the daily path. Modes
1, 2, 3 and 6 still need them, so don't delete them — but nothing in mode 7
depends on them, which means their open bugs (mode 3's cursor blink, the
publish-rate discrepancy, PROBLEM 1's capture stall) block nothing.

---

## Third seat

Mode 7 does not scale directly: each seat needs its own always-composited home
for its client, and it only has one physical panel to give. **Mode 6 is the
shape for seat C.**

- `vdd_settings.xml` → `<count>2</count>` gives a second virtual display
- each seat gets its own client, `seats.toml` entry and physical panel
- seat B stays on mode 7 (its own panel), seat C uses a virtual display + mirror

Watch CPU before promising it to anyone — each seat is another `sdl-freerdp`,
and seat C adds a capture path and a mirror on top.

The virtual display is already permanent via
`devcon install ... "Root\MttVDD"` → `ROOT\DISPLAY\0000`.

---

## Mode 4 — research, not development

Mode 4 would give seat B's *session* its own display, removing the client from
the pixel path entirely. Still architecturally the nicest answer. But mode 7
already removed the machinery mode 4 was going to remove, so what is left is
latency and elegance rather than capability.

**Status:** `0xD000000D` — UMDF refusing every IddCx driver at load level 0.
About fifteen causes eliminated with evidence. Microsoft's own sample loaded on
this machine on the morning of 2026-08-17 and would not by that evening,
unchanged. The environment moved and the differential became unreadable.

**Do not resume on this machine without a clean baseline.** That is the one
thing those sessions proved conclusively.

Paths, best first:

**The RDP IDD angle — unexplored, and the most promising.**
`SWD\REMOTEDISPLAYENUM\RDPIDD_INDIRECTDISPLAY` loads fine here; five
`rdpidd.inf` instances sit in the Display class. Microsoft's own remote-session
IDD works on this machine *today*. Nobody has asked whether an ordinary RDP
session's display can be captured directly rather than through the client's
framebuffer. That would be mode 4's benefit with no custom driver at all.

**Ask VDD's maintainers.** They ship a working IddCx driver and have a
Discussions board. *"Has anyone made this a remote-session driver?"* is two lines
to people who already solved the loading problem. Costs nothing.

**OSR Online, NTDEV.** Where display driver developers answer. The question is
now well-formed: same signed binary loads on one devnode and not another,
`0x1f / 0xc0000001`, class key correct, `ConfigFlags 0`, fifteen causes
eliminated.

**A VM for the baseline.** A mode 4 confined to a VM is worthless as a product —
but the VM is a *measurement*. It answers "is it this machine or the method?",
which decides whether a reinstall is worth an hour. Nothing else can answer it.

**WinDbg on `WUDFHost`.** `HostProcessDbgBreakOnStart` under
`HKLM\SYSTEM\CurrentControlSet\Control\WUDF`, attach a *user-mode* debugger. The
definitive tool, never used, needs a clean machine first.

**A KMDF display miniport.** Sidesteps the class extension entirely — no
`UmdfExtensions`, no `IddCx0102`, no `WUDFHost`. `0xD000000D` cannot follow you
there. Considerably more work; you would be writing a real display driver.

And the honest one: **Windows MultiPoint Server did this natively and Microsoft
retired it.** There may be no supported path because it was removed deliberately.

See `MODE4-STEP2.md` for the full procedure if resumed.

---

## Mode 5 — dropped

Its goal was a console-side IddCx driver. VDD is one, working and permanent.
`iddseat.cpp` would add per-seat EDID and mode control; `vdd_settings.xml`
covers resolutions. Left in the repo as history.

---

## The bigger question — Linux

`systemd-logind` does multiseat natively. `loginctl attach seat1 <sysfs-path>`,
one seat per GPU output, input devices assigned by udev. No RDP loopback, no
capture pipeline, no shared ring, no driver signing, no class extensions, no
`0xD000000D`.

Everything Hydra fights exists because **Windows makes this hard**, not because
the design is wrong.

Hydra remains valuable as the Windows answer: the students' software, the
teaching materials and the classroom's expectations are all Windows-shaped. But
if the goal is two good seats rather than making Windows do it, Linux is the
better engineering and it is a config exercise rather than a driver project.

---

## Recommended order

1. **Use mode 7 on H.264 for the term** (`hydra7-avc.ps1`).
2. **Check text sharpness** — the one H.264 question still open.
3. **Scope TCP-only to the seat** before it surprises a remote session.
4. **The minimize guard and the overlay under `hydrad`** — small, and they remove
   the last ways a lesson can lose seat B's screen or pointer.
5. **Autostart at logon** if the daily command becomes annoying.
6. **Seat C via mode 6** if and when a third student needs one.
7. **Linux multiseat** in a quiet week, as the real answer.

Mode 4 stays in `MODE4-STEP2.md` for whoever picks it up — you, or someone else,
or a future assistant who reads the repo before theorising.

---

## For whoever reads this next

Two rules earned expensively across these sessions:

**Read the repo and `git log` before reasoning.** Commit messages run ahead of
every `.md` here. Nearly every wrong turn — and there were many — was a guess
made before looking at a file that already had the answer.

**One boot-affecting change per reboot.** The 2026-08-12 boot carried four at
once and cost an OS reinstall with no way to tell which was responsible.
