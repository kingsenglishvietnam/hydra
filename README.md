# Hydra — multiseat for Windows

Two people, one PC, at the same time. Each seat has its own keyboard, mouse,
display and audio output, fully isolated from the other.

A free alternative to [ASTER](https://www.ibik.ru/) on consumer Windows, built
because commercial multiseat for Windows has collapsed to a single surviving
product.

| | Seat 1 | Seat 2 |
|---|---|---|
| Session | console | RDP |
| Input | wired keyboard + mouse | wireless keyboard + mouse |
| Display | laptop screen | external monitor |
| Audio | laptop speakers | monitor speakers |

## How it works

**Input** — [Interception](https://github.com/oblitum/Interception) captures the
seat's devices in the console session, matched by *hardware ID* rather than
enumeration index so they survive reboots. `seat_router` forwards events over
loopback TCP to `seatB_agent`, which injects them into the seat's session with
`SendInput`. The agent runs as SYSTEM inside that session so it can reach any
desktop — including the lock screen and UAC prompts.

**Display** — `session_capture` runs in the seat's session and duplicates its
desktop with the Desktop Duplication API, compositing the mouse cursor into each
frame (DDA deliberately excludes it). Frames travel to the console session as
pixels in shared memory, and `mirror` scales them onto the panel.

```
seat's session                    console session
──────────────                    ───────────────
session_capture                        mirror
  DDA + cursor  ──▶ Global\...._pix ──▶  upload,
  composited        shared memory        scale,
                    (seqlock)            present
```

Shared memory rather than a shared D3D11 texture because **textures don't cross
a terminal-services session boundary** — `OpenSharedResourceByName` resolves the
name and then fails `E_INVALIDARG` whatever access flags you pass.

**Audio** — no virtual cable and no extra process. On `audiomode:i:0` the seat's
session has no real audio hardware; `mstsc.exe` in the console session plays its
sound. So a single per-app output assignment does the isolation: system default
to the laptop, `mstsc.exe` to the monitor.

## Running it

```powershell
.\hydra-start.ps1
```

Sequences the whole stack: launches the RDP client, waits for the seat user to
be *logged in* (not merely connected), starts the service, waits for capture to
publish, starts mirror, and primes the audio endpoint.

## Documentation

**[Hydra-Guide.md](Hydra-Guide.md)** — build, configure, daily operation,
troubleshooting, and a list of everything that was tried and doesn't work.

That last section is the important one. Thirteen approaches were built, tested on
hardware and rejected — remote-session IDD drivers, process loopback audio,
endpoint loopback, three different ways of launching mirror, restarting the audio
service from four different security contexts. Each is recorded with the reason
it failed, so nobody repeats them.

## Requirements

Windows 11 Pro · RDP-Wrapper or [TermWrap](https://github.com/llccd/TermWrap) for
concurrent sessions · Interception · Visual Studio Build Tools (x64)

A Windows update that replaces `termsrv.dll` will break concurrent sessions until
the wrapper is updated. That's the main operational risk.

## Status

Working: per-seat input surviving desktop switches, the seat's real desktop on
its monitor with a cursor that can't be occluded by the Start menu, isolated
audio on both seats simultaneously, one-command startup.

Untested: more than two seats. `seats.toml` takes multiple `[[seat]]` blocks and
the supervisor loops over them, so it's expected to work — it just needs another
monitor and a cable.
