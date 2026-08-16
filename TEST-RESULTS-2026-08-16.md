# TEST-RESULTS-2026-08-16.md

All three modes pass. Six real bugs found and fixed, two of which had been
misdiagnosed for days.

---

## The fixes

### 1. `mirror` was DPI-unaware — the big one

`mirror.cpp` called **`SetProcessDPIAware()`**, the Vista-era API that sets
*system* DPI awareness. On a mixed-DPI machine that returns every monitor rect
virtualised against the **primary** monitor's scale, so DISPLAY2 (a real
1920x1080 panel) reported **6720x3780**.

`mirror` then built its swapchain at 6720x3780 and resized down on the first
frame, leaving a mismatched present path for the whole session.

```c
if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    SetProcessDPIAware();
```

**Result: mode 1 went from FAIL on video and cursor to PASS on both**, and
frame rate dropped from 98.2 fps to 25.7 — the 98 was frames being churned
through a stretch that no longer happens.

Hours were spent looking for 350% display scaling that was never set. Windows
was at 100% throughout.

**Corollary:** `[System.Windows.Forms.Screen]::AllScreens` is also DPI-unaware
and reports DISPLAY1 as 926x617 for a 3240x2160 panel. `clip_console.exe` opts
into Per-Monitor-V2 and reports true pixels. Never do arithmetic on the scaled
numbers.

### 2. Seat 2 cursor stutter — a timing collision between our own constants

`seatB_agent.c` rate-limited its desktop re-attach retry to **500 ms**, and
`STALL_MS` is **800 ms**. Line 17 of that file explains why they interact: while
inside `SendInput` the agent *"isn't reading the socket at all"*. Two
consecutive failed retries therefore blow past the 800 ms `SO_RCVTIMEO`, the
agent tears down its router connection, reconnects, and logs `clearing stuck
modifiers` — dropping input each time.

Retry limit **500 ms → 120 ms**. Recovery now fits inside the stall budget.
Worst case is ~8 attempts/sec on a genuinely blocked desktop, still bounded,
which is what the limit was for.

**Result: mode 2 cursor went from FAIL to PASS.**

### 3. `hydrardp.c` no longer compiled — unrelated to any of our changes

MSYS2's FreeRDP package moved when MSYS2 was reinstalled. `windows.h` (via
`winsock2.h`) defines `NIIF_*` as macros from `shellapi.h`; FreeRDP's `rail.h`
uses the same names as enum members, so the enum expands to
`0x00000000 = 0x00000000` and gcc reports *"expected identifier before numeric
constant"*.

`#undef NIIF_*` between `winsock2.h` and the first FreeRDP header.

Confirmed unrelated by building the **unpatched backup**, which failed
identically. `dist\hydrardp.exe` had been running as an 8/11 binary built
against the older headers; nobody had rebuilt it since.

### 4. `display_mode = "client"` produced a white box

`hydrad` creates and **zeroes** the meta section for every seat regardless of
mode (`hydrad.cpp:283-285`), but in `client` mode it launches no capture agent —
and **`hydrardp.c` never touched the meta section at all**. `mirror` reads meta
first for dimensions and format; with `ready=0` it draws blank however many
frames are in the pixel ring.

`hydra_open_pixels` now opens the meta section and fills it in
`session_capture.cpp:127-137`'s exact order, `ready = 1` last after a
`MemoryBarrier()`. `luid` stays 0 — `mirror` reads that as "default adapter",
correct here because the pixel transport is a CPU copy, not a shared texture.

This is why the old working mode 3 recipe killed `session_capture` **manually**:
it let capture populate meta first, then got out of the way. `client` mode
removed the initialisation along with the producer.

### 5. `-suppress-output` and `/scale:140` wired into `hydra-start.ps1`

`-suppress-output` stops the client sending the Suppress Output PDU when
covered. `/scale:140` sets the session's own DPI so DISPLAY2 can sit at 100%
and `mirror` runs 1:1 — the seat's UI stays large without any host-side
scaling.

`teacher.rdp` gains the mode 1 equivalent: `desktopscalefactor:i:140` plus
`devicescalefactor:i:100` (mstsc ignores the file if the second is missing).

### 6. `audio-pin.ps1 -Apply` threw on every launch

*"RegistryKey.SetValue does not support arrays of type 'Object[]'"*. Two bugs on
one line: `-Path` treats `[`, `]`, `?` as wildcards and audio endpoint key names
are full of them, and `$e.Value` is not guaranteed to be a string after a JSON
round trip. Now `-LiteralPath` + `[string]` cast + `-Type String`.

### Also

- **Logitech Download Assistant removed** from `HKLM\...\Run` — it ran
  `rundll32 LogiLDA.dll,LogiFetch` in every session including the seat's, and
  installing that driver inside the seat session is the confirmed PROBLEM 1
  trigger. Plus `SearchOrderConfig=0` and `PreventDeviceMetadataFromNetwork=1`.
- **`PromptOnSecureDesktop=0`** — UAC on the secure desktop is a desktop DDA
  cannot duplicate, which produced the black panel with a frozen cursor.
- **`Import-Module VirtualDesktop -DisableNameChecking`** in both launchers.
- **`hydra-view.ps1`**: PS 7.4 defaults `$PSNativeCommandUseErrorActionPreference`
  on, so `mirror`'s *success* message on stderr became terminating under
  `'Stop'`. Bracketed the `--probe` call with `Continue`.
- **Client thumbnail `-Corner TopRight` → `BottomLeft`** — TopRight sat directly
  on the crossing path between console and DISPLAY2.

---

## Test results

| | mode 1 (mstsc) | mode 2 (sdl-freerdp) | mode 3 (hydrardp) |
|---|---|---|---|
| picture on panel | PASS | PASS | PASS |
| video smooth | **PASS** (was FAIL) | PASS | PASS |
| seat cursor | **PASS** (was FAIL) | **PASS** (was FAIL) | PASS |
| keyboard isolated | PASS | PASS | PASS |
| console unaffected | PASS | PASS | PASS |
| audio at monitor | PASS | PASS | PASS |
| survives UAC | PASS | PASS | — |
| no freeze when covered | — | PASS | n/a (headless) |

Measured, not felt: **25.7 fps / 61.0 cursor-per-sec** (mode 1),
**28.8 fps / 60.3** (mode 2), `ready=1`, `stalled=0` throughout.

---

## `test-modes.ps1` rev 3

- Sets `display_mode` per mode and runs `setup.ps1`. Getting this wrong caused
  several false failures — mode 2 needs `capture`, mode 3 needs `client`.
- **Measures** instead of asking: reads the pixel ring twice over six seconds
  and reports fps, cursor/sec, `ready` and `stalled` as numbers. "Glitchy" is
  not a test result.
- Waits on the `hydractl` pipe instead of a fixed sleep. A six-second sleep was
  racing `hydra-start.ps1` and reporting all six helpers as FAIL while they
  were demonstrably running.
- Preflight verifies `-suppress-output`, `/scale:140`, the Logitech removal,
  `SearchOrderConfig` and `PromptOnSecureDesktop`.

### Harness bugs found and fixed

`ReadRing` used `2>&1 | Out-String` against `hydra-shm.ps1`, which writes via
`Write-Host` — that goes to the **information stream** in PS 7, so nothing was
captured and every measurement came back `-1`. Fixed with `6>&1`.

The agent-restart check counted `clearing stuck modifiers` banners in the log
tail, which accumulate across every run. `hydrad.log` proved the agent launches
**once** and stays — those are router reconnects within one process, not
restarts. Now reported as information rather than a failure.

---

## Still open

**Mode 3 cursor does not track.** Documented and expected: RDP sends no pointer
position to a client that generates no input, so position comes from `agent:B`
via `curSeq` while the image comes from RDP.

**Mode 4 — `0xD000000D`.** UMDF refuses the driver at level 0 before
`DriverEntry`. Driverless mode 4 was tested and ruled out on 08-14: with no IDD
staged the provider session authenticates (4624, Logon Type 10, no 4625) and
stalls at `LogonUI` forever, Winlogon 7001 never fires. Next instrument is the
WUDF framework verifier.

**PROBLEM 5, the reboot tax.** `hydrardp` dying leaves the wrapper holding a
session only a reboot clears. ~40 lines of supervisor. Not written.

---

## Method note

Two things that repeatedly wasted time, worth naming:

**Reading the source settled in one command what inference got wrong over
many.** The DPI bug, the 500/800 ms collision, and the missing meta write were
each found by reading the file rather than theorising. Every wrong turn — the
GPU driver version, the codec, display scaling, two-producer contention — was a
guess made before looking.

**Log messages are not evidence of what the code does.** `mirror`'s
`backbuffer 6720x3780 -> 1920x1080 (DXGI stretches to panel)` prints the *old*
size in both slots and reads as a live stretch; it is actually a one-time
correction. Twenty minutes went into chasing a stretch that had already stopped.
