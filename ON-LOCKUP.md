# When it locks up — run this FIRST

Before restarting anything. A restart destroys the evidence, and mode 2 has
locked up several times with no diagnosis captured.

```powershell
cd C:\Programs\hydra; .\dist\hydractl.exe status; Get-Content C:\ProgramData\Hydra\logs\capture_B.log -Tail 6; Get-Process sdl-freerdp, mstsc, hydrardp, mirror -ErrorAction SilentlyContinue | Select-Object Name, Id, MainWindowTitle, WorkingSet; query session
```

Copy the whole output somewhere before doing anything else.

---

## Reading it

**`capture:B` shows `*** STALLED: no display in session`**
The capture agent is attached to the seat's desktop but `EnumOutputs` returns
nothing -- the session has lost its duplicatable display. Restarting capture does
NOT recover it; only a full logoff and reconnect does.

**`capture_B.log` repeats `attached to WinSta0` / `attached to interactive input
desktop` without ever reaching `duplicating output`**
Same thing: it is looping, not idle. A quiet log here is not health.

**`capture_B.log` ends at `entering capture loop` and nothing since**
Capture is fine. The problem is downstream, or the desktop is genuinely idle.
Confirm with a real measurement rather than the eye:

```powershell
.\dist\mirror.exe B --probe 15 56789
```

**No `sdl-freerdp` process**
The client died and took the seat's session with it. The panel is showing a
stale frame.

**`sdl-freerdp` running but its window minimized or covered**
Mode 2 only: a client that is not visibly composited stops requesting screen
updates, so the seat's desktop stops being composed and the panel freezes on its
last frame. Windows still reports such a window as visible, so nothing detects
it. Restore it:

```powershell
.\minify-mstsc.ps1 -Process sdl-freerdp -TopMost -Width 320 -Height 200 -Corner TopRight
```

**Mirrors at ~7-15 MB instead of 70-98 MB**
They started before anything was publishing. Restart them, not the whole stack.

**`query session` shows the seat as `Disc`, or a `ConnQ` entry**
The RDP stack is wedged. `ConnQ` that `reset session` cannot clear needs a
REBOOT.

---

## Then recover

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc -ErrorAction SilentlyContinue | Stop-Process -Force; Stop-Service Hydra
```

```powershell
query session
logoff <the seat's ID>
```

```powershell
cd C:\Programs\hydra; .\hydra-start.ps1
```

---

## Worth recording each time

Date, which mode, how long it had been running, what the diagnostics said, and
what you were doing when it stopped. Mode 2 locking up "randomly" is the most
important open problem -- more important than the video codec -- and a handful of
captured lockups will show the pattern that guessing has not.
