# Hydra — the two modes

Replaces the earlier three-mode description. mstsc still works but there is no
longer a reason to choose it.

---

## Mode 2 — sdl-freerdp + Desktop Duplication.  **Use this to teach.**

```powershell
.\hydra-start.ps1
```

One command. Pixel-perfect video, because the seat's real desktop is captured
rather than encoded and decoded.

The RDP client window must stay **visible and uncovered**. Covered or minimized,
it stops requesting screen updates, the desktop stops being composed, and the
panel freezes. `hydra-start.ps1` parks it as a 320x200 topmost thumbnail for
exactly this reason.

If your cursor reaching that thumbnail moves the seat's cursor, the window is
forwarding your input -- move it out of the way:

```powershell
.\minify-mstsc.ps1 -Process sdl-freerdp -TopMost -Width 320 -Height 200 -Corner BottomLeft
```

---

## Mode 3 — hydrardp, the headless client.  **Development.**

```powershell
.\hydra-view.ps1 -Desktop 2
```

One command. Clears conflicts, starts the service, opens the client for the
password, **waits for frames to actually exist**, then starts both mirrors and
moves the fullscreen seat view to virtual desktop 2. `Win+Ctrl+Left/Right`
switches.

No window anywhere in the capture path, so the freeze that shaped days of this
project is impossible rather than avoided -- and no session boundary, so the
cross-session texture problem does not arise either.

**What it still gets wrong:**

| Gap | Why |
|---|---|
| Video is glitchy | No codec. Plain bitmap updates, which look poor on moving images. `/gfx` crashes with a null call before the first frame; three hypotheses tried, all wrong. Next step is reading FreeRDP's client source, not another patch. |
| The cursor does not track | It renders correctly -- compositing works -- but RDP sends no pointer POSITION to a client that sends no input. Fix: `agent:B`, which runs as SYSTEM inside the session, publishes `GetCursorPos` into the shared header. Designed, not built. |

---

## Never

- **Run both modes at once.** Two clients on one session, or two producers on
  one pixel ring, wedges the RDP stack -- and that costs a reboot, not a restart.
  Stop one fully before starting the other:

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc -ErrorAction SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; query session
```

- **Test shutdown with `Stop-Process -Force`.** That is `TerminateProcess`; no
  user-mode handler can intercept it, so it proves nothing about the crash
  handler. Use **Ctrl+C** in the client's window.

- **Extract a whole-tree zip over a working tree.** One stale zip downgraded the
  sources and cost a `git checkout -- .` recovery. Individual files only.

---

## Retired, with reasons

- **`cursorfence`** — a mouse hook to stop the cursor crossing onto the panel.
  Unnecessary: the two displays sit at `0,0` and `11340,0`, about 8100 pixels
  apart, so the desktop is not contiguous and the cursor cannot walk across. The
  real leak was the RDP client thumbnail forwarding input.
- **`ClipCursor` confinement** — Windows releases the clip on every foreground
  change, so a background helper cannot hold it. Re-applying on a timer leaves
  gaps the cursor escapes through.

---

## Mouse handedness

Seat B's button mapping has historically come out OPPOSITE to the console's,
requiring a manual swap every time. Two places can compensate, and only one
should:

**seatB_agent.c MOUSE_MAP** (currently swapped) -- affects the STUDENT's
wireless pair, which reaches the seat via seat_router -> agent:B -> SendInput.

**SwapMouseButtons in the seat user's profile** -- Settings > Bluetooth &
devices > Mouse > Primary mouse button, set INSIDE the seat session. Read by
explorer at logon, so it needs a real logoff/logon, not just a client relaunch.

Your console mouse crossing onto the seat's panel uses NEITHER -- that pointer
belongs to the console session and follows the console's own setting.

If a mouse comes out backwards, one layer is compensating twice. Remove the
Windows setting first; it is per-profile and easy to lose track of.


## Smart App Control

On 2026-08-21 Smart App Control began blocking sdl-freerdp.exe with
'An Application Control policy has blocked this file'. The binary had not
changed -- PowerShell had updated from 7.6.4.0 to 7.6.5.0 and SAC re-evaluated
what it was launching. CodeIntegrity/Operational events 3033/3077/3118, policy
ID {0283ac0f-fff1-49ae-ada1-8a933130cad6}.

SAC will never accept an unsigned FreeRDP build or our own binaries, so it has
to be OFF. Settings > Privacy & security > Windows Security > App & browser
control > Smart App Control > Off.

TURNING IT OFF IS PERMANENT -- it cannot be re-enabled without reinstalling
Windows. Defender is unaffected.

Check with:
  Get-CimInstance Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard | Select UsermodeCodeIntegrityPolicyEnforcementStatus
0 = off, 1 = audit, 2 = enforced.


## Locking

Seat B locking affects only seat B. Sessions are independent.

Locking the CONSOLE takes both seats down. That is not a Hydra bug: the
Winlogon secure desktop is a machine-wide state, and while it is the input
desktop nothing else renders or receives input. Seat B's session keeps running
underneath, but its display and input are suspended until the console unlocks.

Practical: do not lock the console mid-lesson. Lock seat B instead, or use a
screensaver on the console panel only.

Seat B CAN be unlocked with teacher's password from the wireless keyboard --
agent:B re-attaches on the desktop switch ('input desktop changed; re-attached
and recovered' in agent_B.log). The Winlogon-by-name fallback added 2026-08-21
has never needed to fire; OpenInputDesktop with GENERIC_ALL succeeds.


## Mode 6 vs mode 7 for video

Mode 6 tears less than mode 7 on video playback. Tested 2026-08-21, same clip,
same panel.

WHY. Mode 7's client presents straight to the panel from an SDL window -- no
vsync, no swapchain, no frame pacing. It blits whenever a frame arrives off the
RDP stream, which is not paced to the display.

Mode 6 goes client -> virtual display -> DDA -> seqlock ring -> mirror, and
mirror presents through a DXGI SWAPCHAIN, which is vsynced. The ring also
decouples producer from consumer, so mirror presents complete frames at the
panel's rate rather than at the stream's.

So the machinery mode 7 removed was doing something after all: it is a frame
buffer with a real presentation stage on the end.

PRACTICAL: mode 7 for ordinary lessons -- simpler, one command, nothing to go
wrong. Mode 6 for video-heavy lessons. Both are one command and both work.
Remember display_mode: 'off' for mode 7, 'capture' for mode 6.
