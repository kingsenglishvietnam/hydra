# POST-REBOOT — finish 2026-08-16

Everything below assumes the Hydra Shell (elevated, x64 Native Tools) at
`C:\Programs\hydra`.

---

## 0. Sanity check first

```powershell
cd C:\Programs\hydra; .\test-modes.ps1 -PreflightOnly
```

All green expected. If `-suppress-output` or `/scale:140` fail, the
`hydra-start.ps1` edits did not survive — restore from
`hydra-start.ps1.bak-*` and reapply.

---

## 1. Apply the cursor fix (if not already applied)

```powershell
Select-String -Path C:\Programs\hydra\rdp\hydrardp.c -Pattern 'SetDefault means' | Select-Object LineNumber
```

A hit means it is already in. Nothing to do — skip to §2.

Otherwise:

```powershell
Copy-Item C:\Programs\hydra\rdp\hydrardp.c "C:\Programs\hydra\rdp\hydrardp.c.bak-$(Get-Date -f yyyyMMdd-HHmmss)"
```

```powershell
$f='C:\Programs\hydra\rdp\hydrardp.c'; $t=[IO.File]::ReadAllText($f); $t=$t -replace '(?m)^\s*\(void\)context;\r?\n\s*return TRUE;\s*/\* keep whatever we last had \*/', @'
    /* SetDefault means "show the standard arrow" -- it is a SHOW, not a no-op.
     * set_null clears curVisible and nothing here put it back, so once an app
     * hid the cursor it stayed hidden until a NEW pointer IMAGE arrived. RDP
     * caches shapes, so returning to a shape already sent does not necessarily
     * reach hydra_pointer_set -- and the cursor vanished over text fields, mid
     * drag, and between windows. That is the "blinky cursor" in mode 3. */
    HydraContext* h = (HydraContext*)context;
    EnterCriticalSection(&h->curLock);
    h->curVisible = TRUE;
    LeaveCriticalSection(&h->curLock);
    return TRUE;
'@; [IO.File]::WriteAllText($f,$t); Get-Content $f | Select-Object -Skip 535 -First 20
```

**GATE:** the printed block must show `h->curVisible = TRUE;` inside
`hydra_pointer_set_default`. If it does not, restore the backup and stop.

```powershell
Get-Process hydrardp -EA SilentlyContinue | Stop-Process -Force
.\build-rdpclient.ps1
```

```powershell
Get-Item C:\Programs\hydra\dist\hydrardp.exe | Select-Object Length, LastWriteTime
```

Timestamp must be from this build. Fallback at any point:
`Copy-Item dist\hydrardp.exe.GOOD-20260811-1607 dist\hydrardp.exe -Force`

---

## 2. Test mode 3 — WINDOWED, not fullscreen

**Do not run `mirror B \\.\DISPLAY2` by hand on a machine you are sitting at.**
That is borderless fullscreen with no exit path; it trapped the console twice on
2026-08-16 and cost two hard power-offs. The fullscreen form is for a real seat
with a student at it.

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc -EA SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; query session
```

`logoff <teacher id>` if one lingers.

```powershell
Select-String -Path C:\Programs\hydra\seats.toml -Pattern '^display_mode'
```

Mode 3 wants `client`. If it says `capture`:

```powershell
(Get-Content C:\Programs\hydra\seats.toml -Raw) -replace '(?m)^display_mode = "capture"', 'display_mode = "client"' | Set-Content C:\Programs\hydra\seats.toml -NoNewline
.\setup.ps1
```

Shell 1 — service first, it creates the shared sections:

```powershell
cd C:\Programs\hydra; Stop-Service Hydra -EA SilentlyContinue; Start-Sleep 2; Start-Service Hydra; $env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher
```

**GATE:** `meta published: 1920x1080 fmt=87 gen=N` must appear. That is the
08-16 meta patch working. Without it `mirror` draws a white box.

Wait for publishes climbing. Shell 2, windowed:

```powershell
cd C:\Programs\hydra; .\dist\mirror.exe B --window 1600x900
```

### The cursor test

Hover over a **text field** in seat B, then drag a window. Those are the
reliable triggers — an app hiding the cursor is what `set_null` responds to.

Steady cursor = fixed. Still vanishing = the theory was wrong and
`hydra_pointer_set` is not being reached on cached shapes either.

Measure it rather than judging by eye:

```powershell
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

Healthy, with activity in the seat: `frame` +400-500 over 4s, `curSeq` +240,
`ready=1`, no `STALLED` line. **Both `frame` and `cur` position must change** —
a static desktop publishes nothing and that is not a failure.

---

## 3. Full test run

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc -EA SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; query session
```

`logoff <teacher id>`, then:

```powershell
.\test-modes.ps1 -Mode 2
```

Mode 2 is the teaching mode — run it last of the three so the machine is left in
that state. Full stop and `logoff` between every mode.

The script now sets `display_mode` per mode itself, waits on the `hydractl`
pipe, and measures fps / cursor-per-sec / ready / stalled as numbers.

---

## 4. Commit

```powershell
cd C:\Programs\hydra
git status --short
git add -A
```

```powershell
git commit -m "all three modes pass: DPI, agent timing, NIIF headers, client-mode meta, cursor visibility

mirror.cpp: SetProcessDPIAware() -> SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2).
The old call sets SYSTEM awareness, so on mixed DPI every monitor rect comes back
virtualised against the PRIMARY monitor's scale -- DISPLAY2 reported 6720x3780
for a 1920x1080 panel and mirror built its swapchain at 3.5x, resizing down on
the first frame. Mode 1 video and cursor FAIL -> PASS; frame rate fell 98.2 ->
25.7 fps because the 98 was frames churned through a stretch that no longer
happens. Windows was at 100% scaling throughout.

seatB_agent.c: desktop re-attach retry 500ms -> 120ms. STALL_MS is 800ms and the
agent does not read its socket while inside SendInput (see its own comment at
line 17), so two failed retries blew past SO_RCVTIMEO, tore down the router
connection and dropped input on each reconnect. Mode 2 cursor FAIL -> PASS.

hydrardp.c: #undef NIIF_* before the FreeRDP headers. windows.h defines them as
macros from shellapi.h and FreeRDP's rail.h uses the same names as enum members.
NOT ours -- the unpatched backup fails identically. MSYS2's freerdp package moved
when MSYS2 was reinstalled and dist\\hydrardp.exe had been an 8/11 binary since.

hydrardp.c: populate the meta section in hydra_open_pixels. hydrad creates and
zeroes it for every seat, but display_mode=client launches no capture agent and
hydrardp never touched meta -- mirror saw ready=0 and drew a white box however
many frames were in the ring. Filled in session_capture.cpp:127-137's order,
ready=1 last after MemoryBarrier. This is why the old mode 3 recipe killed
session_capture MANUALLY: it let capture populate meta first.

hydrardp.c: hydra_pointer_set_default now sets curVisible = TRUE. It was a no-op
'keep whatever we last had' -- but set_null clears curVisible, so once an app hid
the cursor only a NEW pointer image restored it. RDP caches shapes, so returning
to a known shape need not reach hydra_pointer_set. That is the mode 3 blinky
cursor: it vanished over text fields, mid drag and between windows.

Four earlier explanations for that cursor were wrong and are recorded so nobody
retries them: a 124-vs-61 publish/position rate mismatch (the composite reads
curX/curY at composite time, so it is never stale); missing cursor erasure via
prevCurX/Y/W/H (those are dead fields from the removed damaged-rectangle path --
the publish is a full-frame copy); skipping the composite when curSeq has not
advanced (would drop the cursor entirely, since the composite draws into the
ring on every publish); and incomplete pointer registration (all six callbacks
are registered).

hydra-start.ps1: -suppress-output and /scale:140 wired in; thumbnail corner
TopRight -> BottomLeft (it sat on the crossing path to DISPLAY2).
teacher.rdp: desktopscalefactor:i:140 + devicescalefactor:i:100.
audio-pin.ps1: -LiteralPath + [string] cast -- -Path treats [ ] ? as wildcards
and audio endpoint key names are full of them.
hydra-view.ps1: PS7.4 native-stderr made mirror's SUCCESS message terminating.
Removed Logitech Download Assistant from HKLM Run (installing that driver inside
the seat session is the confirmed PROBLEM 1 trigger); SearchOrderConfig=0;
PreventDeviceMetadataFromNetwork=1; PromptOnSecureDesktop=0.

test-modes.ps1 rev 3: sets display_mode per mode, waits on the hydractl pipe
instead of sleeping, and MEASURES fps / cursor-per-sec / ready / stalled rather
than asking whether it looks glitchy. Two harness bugs fixed: ReadRing used 2>&1
against a Write-Host script so every measurement returned -1 (needs 6>&1), and
the agent-restart check counted log lines accumulating across runs -- hydrad.log
proves the agent launches once and stays."
```

```powershell
git push
git tag -a all-modes-pass-2026-08-16 -m "Modes 1-3 all pass. mirror DPI awareness, agent retry timing, NIIF header collision, client-mode meta population, pointer visibility on SetDefault. Mode 4 still blocked on 0xD000000D."
git push origin all-modes-pass-2026-08-16
```

Also drop `TEST-RESULTS-2026-08-16.md` into the repo before committing if it is
not already there.

---

## Still open

**Mode 3 publish rate.** The throttle comment says EndPaint fires ~280/sec and
is capped to ~60/sec, but 124 fps was measured. Two paths now reach
`hydra_end_paint` — EndPaint and the gfx `EndFrame` callback — and the 16 ms
gate may only apply to one. Not urgent; more frames than the panel can show is
wasted work rather than a fault.

**Mode 4 — `0xD000000D`.** UMDF refuses the driver at level 0 before
`DriverEntry`. Driverless mode 4 was tested and ruled out 08-14: with no IDD
staged the provider session authenticates (4624, Logon Type 10, no 4625) and
stalls at `LogonUI` forever — Winlogon 7001 never fires. Next instrument is the
WUDF framework verifier (`WdfVerifier.exe`), or build Microsoft's own IddCx
sample to separate environment from `iddseat.cpp`.

**PROBLEM 5, the reboot tax.** `hydrardp` dying leaves the wrapper holding a
session that only a reboot clears. ~40 lines of supervisor. Not written.

---

## Rules that earned their place on 08-16

- **Read the source before theorising.** The DPI bug, the 500/800 ms collision,
  the missing meta write and the cursor visibility asymmetry were each found by
  reading the file. Every wrong turn — GPU driver version, codec, display
  scaling, two-producer contention, four separate cursor theories — was a guess
  made before looking.
- **Log messages are not evidence of what the code does.** `mirror`'s
  `backbuffer 6720x3780 -> 1920x1080 (DXGI stretches to panel)` prints the old
  size in both slots and reads as a live stretch; it is a one-time correction.
- **Never launch `mirror` fullscreen for testing.** Use `--window 1600x900`.
- **Stop the service before `build.ps1`** or the running exes cause LNK1104.
- **Verify a patch landed before building.** One edit was silently lost to a
  restore and produced twenty minutes of debugging a binary that did not contain
  it.
