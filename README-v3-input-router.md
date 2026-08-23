# multiseat input router (v3)

The missing piece of neo_multiseat: **per-device input isolation**. Your
RDP-Wrapper setup already gives you extra logged-in sessions displayed on
extra monitors. What it can't do is bind one physical keyboard/mouse to each
of those sessions and the rest to the console. This does exactly that, and
nothing else.

It does **not** replace RDP for *display* — an extra seat's pixels still come
through its mstsc window (that's how a second session reaches monitor 2 on a
one-display GPU). It replaces RDP for *input*, so each seat owns its own
keyboard and pointer with no shared cursor. Net result: properly
input-isolated seats. Good for multi-person productivity; the extra seats'
displays keep RDP's latency, so it's not for twitch gaming.

```
   seat A devices ─┐                         ┌─ console session (seat A)
                   │   ┌─────────────────┐   │   pointer + keyboard, native
   [Interception]──┼──▶│  seat_router.c  │───┘
   seat B devices ─┤   │  (console sess) │──────┐ loopback TCP per seat
   seat C devices ─┘   └─────────────────┘      │ (9-byte records,
                                                │  non-blocking,
                        one port + one agent    │  200 ms keepalive)
                        per extra seat          ▼
                       ┌─────────────────┐   ┌─────────────────┐
                       │ seatB_agent.exe │   │ seatB_agent.exe │  ...
                       │  (seat B sess)  │   │  (seat C sess)  │
                       └─────────────────┘   └─────────────────┘
                          SendInput ▼            SendInput ▼
                         seat B session         seat C session
```

`seat_router.c` sees every stroke. Seat A's strokes pass through (console
works normally). Each extra seat's strokes are **captured and dropped** — so
they never reach the console — and forwarded to that seat's agent, which
replays them with `SendInput` *inside* the seat's session. Because
`SendInput` targets the calling thread's session, each seat's input lands in
that seat and only that seat. That is the trick that removes the
shared-cursor problem: an extra seat no longer depends on its mstsc window
holding focus.

All components are native C. No Python in the runtime path.

## What's new in v3

v2 made the system survive unattended operation (non-blocking drop-on-full
sockets, 200 ms keepalive + 800 ms stall detection, clamp rehook watchdog,
topology tracking, `respawn.exe`). v3 closes the gaps v2 documented:

- **Multi-seat.** The router takes `<kbd> <mouse> <port>` triples and drives
  up to four extra seats (B, C, …), each with its own agent, port, and
  independent backpressure state. The old two-argument form still works.
- **Hung-agent recovery.** The v2 keepalive detects a dead *router*
  connection, but an agent wedged inside its own process (e.g. a hung
  `SendInput`) isn't reading the socket at all, so no recv timeout can fire —
  and it looks alive to a plain supervisor. A cross-session kill from the
  router would need privileges (seats run as different users), so the fix
  lives where none are needed: each agent runs a watchdog **thread inside
  itself** that checks the pump keeps making progress (the heartbeat
  guarantees a tick at least every ~800 ms on a healthy link) and, after 5 s
  of stillness, terminates its own process nonzero — which `respawn.exe`
  *can* see, and restarts. The router and clamp carry the same pattern for
  their own wedge-able paths.
- **Instant secure-desktop recovery.** The clamp registers a WinEvent hook
  for `EVENT_SYSTEM_DESKTOPSWITCH`: the moment a UAC prompt / lock screen /
  Ctrl-Alt-Del returns to the normal desktop, the hook is reasserted and the
  cursor snapped back inside — milliseconds instead of v2's up-to-2 s
  watchdog tick.
- **Absolute pointing devices.** Interception's absolute-move flags (tablets,
  some touchpads; 0..65535 normalized coordinates) now travel in spare high
  bits of the wire record and replay as `MOUSEEVENTF_ABSOLUTE`
  [+`VIRTUALDESK`]. Absolute moves coalesce by replacement (newest position
  wins) and never fold across modes.
- **Horizontal wheel.** State bit 0x800 has ridden the wire since v1 but was
  silently ignored on replay; it now maps to `MOUSEEVENTF_HWHEEL`.
- **Stuck-modifier cleanup.** On every (re)connect the agent injects keyups
  for both Shifts, Ctrls, Alts and Win keys — a dropped or lost keyup can no
  longer strand a modifier past the next reconnect, and keyups for keys
  already up are no-ops.
- **Exit-code contract.** `0` = deliberate stop, `2` = configuration error —
  `respawn.exe` restarts *neither* (restarting can't fix a wrong command
  line; v2 would have retried a bad monitor index forever). Anything else is
  a fault and restarts. The hang watchdogs exit `3`.
- **QuickEdit disabled** on the router/agent/clamp consoles. Selecting text
  in a QuickEdit console blocks every write to it; the router logs under the
  same lock its input loop takes, so one accidental drag-select in that
  window would have frozen every extra seat's input until a keypress.

## Prerequisites

- Concurrent sessions already working (RDP-Wrapper / neo_multiseat), with
  each extra seat logged into its own account and shown fullscreen on its
  monitor.
- **Interception** driver installed + reboot: https://github.com/oblitum/Interception
  (`install-interception.exe /install`, then reboot).
- A C compiler (MSVC `cl` shown below). Only the router needs the
  Interception SDK; the agent, clamp and supervisor are pure Win32.

## Build

From an **x64 Native Tools Command Prompt** (Interception SDK unpacked for
the router):

```
cl /O2 seat_router.c /I <sdk>\include <sdk>\library\x64\interception.lib
cl /O2 seatB_agent.c
cl /O2 clip_console.c
cl /O2 respawn.c
```

## Run order

Run components directly for bring-up; wrap them in `respawn.exe` for
unattended use. Bring-up first — get device numbers and the monitor index
sorted before supervising anything.

1. **Start an agent in each extra seat.** Inside that seat's session:
   ```
   seatB_agent.exe                     (seat B, default port 56789)
   seatB_agent.exe 127.0.0.1 56790     (seat C, matching its router triple)
   ```
   Agents retry until the router is up, so order isn't critical.

2. **Identify each seat's device numbers** (console session):
   ```
   seat_router.exe --learn
   ```
   Press keys on each keyboard, wiggle each mouse, note the `dev=` numbers.
   Interception numbers keyboards 1–10 and mice 11–20. Ctrl-C when done.

3. **Run the router** (console session):
   ```
   seat_router.exe <kbd> <mouse> [port]                  (one extra seat)
   seat_router.exe <kbd1> <mouse1> <port1> [<kbd2> <mouse2> <port2> ...]
   e.g.  seat_router.exe 2 12 56789
         seat_router.exe 2 12 56789 3 13 56790
   ```
   Each listed seat's keyboard/mouse now drive that seat; everything else
   stays on the console. Duplicate devices/ports are rejected (exit 2).

4. **(Optional) Confine seat A's cursor** so it can't drift onto the other
   monitors (console session):
   ```
   clip_console.exe            # prints monitors + rectangles + device names
   clip_console.exe 0          # confine seat A to monitor 0
   ```
   `WH_MOUSE_LL` clamp: out-of-bounds moves are blocked inside the input path
   and the pointer is parked at the nearest in-bounds point, so it slides
   along the edge exactly like ClipCursor feels — no reassert gap, no idle
   CPU, no fighting apps that clip the cursor themselves. Per-Monitor-V2 DPI
   aware (correct boundaries on mixed-DPI setups). The rect follows display
   changes automatically; the chosen monitor is tracked by device name, so it
   survives replug and rescale, and a UAC/lock-screen excursion is retrieved
   the instant the desktop switches back.

### Unattended

Wrap each component and autostart *that* (Startup folder / logon Scheduled
Task, in the matching session):

```
Console session (seat A):
   respawn.exe seat_router.exe 2 12 56789
   respawn.exe clip_console.exe 0

Seat B session:
   respawn.exe seatB_agent.exe

Seat C session (if present):
   respawn.exe seatB_agent.exe 127.0.0.1 56790
```

`respawn.exe` restarts on faults with capped backoff (500 ms → 15 s; a run
that stayed up ≥10 s resets it) and honors the exit-code contract: `0`
(deliberate stop) and `2` (config error) stay stopped, everything else — a
crash, or a hang watchdog's self-kill — comes back.

## Wire format

9 bytes, little-endian, packed — identical in `seat_router.c` (`WireEvent`),
`seatB_agent.c` (`WireEvent`) and `seatB_agent.py` (`struct "<BHHhh"`):

| field | type | keyboard            | mouse                                  |
|-------|------|---------------------|----------------------------------------|
| kind  | u8   | `'K'`               | `'M'`                                  |
| a     | u16  | scancode            | Interception state bits + move flags   |
| b     | u16  | key state flags     | wheel `rolling` (as u16)               |
| dx    | i16  | 0                   | relative dx, or absolute x (u16 bits)  |
| dy    | i16  | 0                   | relative dy, or absolute y (u16 bits)  |

Interception button/wheel state occupies bits 0x001–0x800 of `a`; v3 uses the
spare high bits for move mode: `0x1000` = absolute (dx/dy are 0..65535
coordinates, bit-preserved through the i16 fields), `0x2000` = absolute
coordinates span the virtual desktop rather than the primary monitor.

A third `kind`, `'H'`, is the keepalive: the router sends one every 200 ms
with all other fields zero. The layout is unchanged — `'H'` is just a new
value in the `kind` byte — and the agent skips any non-`'K'`/`'M'` kind
explicitly. (A literal zero-*length* send isn't used because TCP delivers no
event for it; the 9-byte no-op record is what actually reaches the peer.)

## Known limits (what v3 deliberately does not fix)

- **Display latency on the extra seats is unchanged** — it's still RDP. This
  tool only fixes input. Fixing display too means a kernel/indirect display
  driver (IddCx territory) presenting each session to a physical output —
  which is, more or less, the product ASTER sells. Out of scope here.
- **A hang inside `interception_wait()` isn't self-detected.** From outside,
  a blocked driver wait is indistinguishable from two idle seats, so the
  router's hang watchdog covers only the socket path (heartbeat thread). If
  the driver call itself ever wedges, input for the extra seats stops until
  the router is restarted by hand. Never observed; noted for honesty.
- **Secure-desktop crossing is retrieved, not prevented.** LL hooks don't run
  on the UAC/lock desktop — by design, so that nothing like this tool can
  interfere with it. The pointer can sit on the wrong monitor for the
  duration of the prompt; it's recovered the instant the desktop switches
  back.
- **Backpressure drops, it doesn't buffer.** If an agent can't keep up, the
  router discards the overflow (counted, logged at most once a second) rather
  than growing an unbounded queue and adding latency. Right trade for a human
  at a keyboard. A mid-connection dropped keyup can stick a non-modifier key
  until it's pressed again; modifiers are cleared on every reconnect.
- **Toggle-key LEDs can lie.** Each seat's NumLock/CapsLock/ScrollLock
  *state* is internally consistent — it's driven entirely by that seat's
  replayed events — but the physical LEDs on a captured keyboard are driven
  by the console session, which never sees those keypresses. Functional
  impact: none; the light is just wrong. Fixing it would mean
  `IOCTL_KEYBOARD_SET_INDICATORS` on the right `KeyboardClass` device with
  admin rights and a fragile device-number mapping — not worth it for a
  cosmetic LED.
- **Absolute-device support is untested by definition** until a tablet or
  absolute touchpad is actually plugged in; the mapping mirrors the kernel's
  `MOUSE_MOVE_ABSOLUTE`/`MOUSE_VIRTUAL_DESKTOP` semantics onto
  `SendInput`'s, which are documented as equivalent.
- **AV may flag the Interception driver** (true of any input filter driver),
  and the RDP-Wrapper concurrent-session licensing caveat still applies —
  Windows client is licensed for one session; this rides on top of your
  existing setup.
- **Untested in the environment it was written in** (no Windows/Interception/
  devices there). Written against the real Interception and Win32 APIs; the
  three pure-Win32 binaries cross-compile clean under MinGW `-Wall -Wextra`
  and the router compiles clean against the SDK headers. Treat the first run
  as a bring-up, not a finished build.

## Python prototypes

`seatB_agent.py` and `clip_console.py` are the original v1 prototypes, kept
because they're short and readable — but the C versions are the ones to run.
They lack everything since (coalescing, keepalive/stall detection, watchdogs,
topology tracking, backpressure handling, absolute devices, hwheel), and
`clip_console.py` additionally has a mixed-DPI coordinate-space bug the C
version fixes (`python.exe` is DPI-unaware). The Python agent ignores the
router's `'H'` keepalives and the v3 move-mode flag bits harmlessly, but
can't act on either. If you'd rather keep the *capture* side in Python, the
`interception-python` package wraps the same driver; the router loop maps
1:1 onto the C.
