# MODES.md — how seat B's picture reaches its monitor

Rewritten 2026-10-11. Seven modes, kept because each one taught something and
the older ones still work as fallbacks. **Use mode 7, launched with
`hydra7-avc.ps1`.**

| Mode | Launcher | Seat B's picture | Status |
|---|---|---|---|
| **7, H.264** | `hydra7-avc.ps1` | mstsc full-screen on the panel; H.264 encoded on Quick Sync, decoded on the GPU | **Use this.** ~26–30 CPU-seconds per minute of video |
| 7 | `hydra7.ps1` | sdl-freerdp full-screen on the panel; RemoteFX on the CPU | Works — the fallback. ~131 per minute of video |
| 6 | `hydra6.ps1` | the client full-screen on a virtual display (VDD), mirrored to the panel | Works. About twice mode 7's CPU; the shape a third seat needs |
| 5 | — | our own IddCx virtual display, `iddseat` | The driver works; not in the daily path |
| 4 | — | the seat's session gets a display of its own, no client at all | Closed — README, "Why mode 4 is closed" |
| 3 | `hydra-view.ps1` | headless client (`hydrardp`) into shared memory, mirrored | Works; development |
| 2 | `hydra-start.ps1` | sdl-freerdp, plus Desktop Duplication inside the seat, mirrored | Works |
| 1 | `hydra-start.ps1 -Client mstsc` | mstsc, plus Desktop Duplication, mirrored | Works; freezes if the client window is covered |

Modes 1–3 and 6 need `display_mode = "capture"` in `seats.toml`; mode 7, either
launcher, needs `"off"`. Change it, then run `.\setup.ps1`. Getting this wrong
is the most common cause of a confusing failure.

---

## Mode 7 on H.264 — `hydra7-avc.ps1`.  **Teach on this.**

```powershell
.\hydra7-avc.ps1          # up
.\hydra7-avc.ps1 -Stop    # down
```

The RDP client *is* the seat's screen: mstsc, full-screen on the seat's panel.
Seat B's desktop is drawn on the Iris Plus, encoded to H.264 4:4:4 by Quick Sync
and decoded on the GPU. No capture, no shared ring, no mirror.

**Once, before the first run** — `VIDEO-H264.md` has the detail:

- the registry recipe: `.\hydra-gpu-probe.ps1 -Apply`, then reboot
- the seat cursor overlay built as `dist\cursor_overlay_seat.exe` (`BUILD.md`)
- AV1 blocked in the seat's browsers
- the audio pin the launcher prints on its first run

**What the launcher does:** preflight (termsrv patched; the H.264 policies set;
UDP off; `bEnumerateHWBeforeSW` absent), the console taskbar off the seat panel,
the Hydra service, mstsc's audio pin, a fresh `.rdp` for the panel it finds,
mstsc made full-screen there through its own window menu, the window pinned to
every virtual desktop, the seat's cursor overlay, then a status check.

**Its limits:**

- mstsc stops drawing when minimized. Win+D on *your* keyboard hides seat B until
  something restores it.
- Text may be slightly softer than under RemoteFX — not compared yet.
- `fClientDisableUDP` makes *every* mstsc connection from this laptop TCP-only.
- Pointers that apps draw themselves come through at 32 px, enlarged.

## Mode 7 — `hydra7.ps1`.  **The fallback.**

The same arrangement with sdl-freerdp. RemoteFX is encoded and decoded on the
CPU, about two processors for full-screen video, more now that the frame cap is
60. `-suppress-output` keeps it drawing even when covered, which mstsc can't.
Its audio pin is `audio-pin.json`.

FreeRDP 3.30.0 can do H.264: `/gfx:avc444` works, decoding on the CPU — about 70
CPU-seconds a minute for the pipeline. `/gfx:avc420` falls back to no H.264, and
`/gfx:h264` crashes.

## Mode 6 — `hydra6.ps1`

The client runs full-screen on a virtual display (MttVDD), so it can never be
covered, minimized or stranded on another virtual desktop; `mirror` copies that
display to the panel. About twice mode 7's CPU and slightly softer, but a virtual
display per seat is the shape a third seat needs.

## Modes 1, 2 and 3 — the capture designs

Desktop Duplication inside the seat's session composites the seat's cursor into
the frame, writes the pixels to shared memory, and `mirror` presents them on the
panel. Mode 1 uses mstsc and freezes if its window is covered or minimized; mode
2 uses sdl-freerdp, which doesn't; mode 3 uses `hydrardp`, a headless client with
no window at all.

## Mode 5 — our own virtual display

`iddseat`, an IddCx driver of our own. It works; mode 6 uses the signed MttVDD
release instead, and nothing in daily use needs either.

## Mode 4 — closed

A remote-session IddCx driver and an RDS protocol provider would have given the
seat's session a display of its own, with no RDP client at all. Both parts work.
`GetInputHandles` wants kernel input miniports, and all they would buy is
removing one user-mode process. `MODE4-STEP2.md` keeps the detail.

---

## Never

- **Run two clients on one session**, or two producers on one pixel ring. It
  wedges the RDP stack, and that costs a reboot, not a restart. Stop one fully
  before starting another — `-Stop` on the launcher, or:

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay_seat -ErrorAction SilentlyContinue |
    Stop-Process -Force
Stop-Service Hydra
query session
```

- **Start straight after a failed connection.** `-Stop`, wait ten seconds, then
  start. A logon that follows a logoff too closely times out.
- **Set `bEnumerateHWBeforeSW`.** It moves seat B onto the 1660 Ti, whose H.264
  encoder crashes `RdpIdd.dll` about 14 seconds in.
- **Mute mstsc in mode 7.** The old "mute mstsc.exe" note belongs to the capture
  modes, where the audio bridge carries the sound. In mode 7 mstsc *is* seat B's
  sound, and Windows remembers the mute indefinitely.
- **Test shutdown with `Stop-Process -Force`.** That is `TerminateProcess`; no
  user-mode handler can intercept it, so it proves nothing about the crash
  handler. Use **Ctrl+C** in the client's window.
- **Extract a whole-tree zip over a working tree.** One stale zip downgraded the
  sources and cost a `git checkout -- .` recovery. Individual files only.

---

## Retired, with reasons

- **The cursor overlay inside seat B** (2026-10-10). Started there with PsExec it
  drew the right cursor, but lost to Task View and vanished on a virtual desktop
  switch. The seat mode runs it from the console session instead.
- **`cursorfence`** — a mouse hook to stop your cursor crossing onto the panel.
  In mode 7, crossing onto the panel is how you reach seat B yourself.
- **`ClipCursor` confinement** — Windows releases the clip on every foreground
  change, so a background helper cannot hold it. Re-applying on a timer leaves
  gaps the cursor escapes through.
- **RDP Wrapper** (2026-09-24) — replaced by `tools/termsrv-guard`.
