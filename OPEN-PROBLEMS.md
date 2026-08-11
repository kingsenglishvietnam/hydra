## 2026-08-10 — session findings

### PROBLEM 4 — the thumbnail hypothesis is wrong

Three window positions tested, all leaked:

| Moved | From | To | Result |
|---|---|---|---|
| `sdl-freerdp` (pid 5124) | (254,127) | (2740,1200) | still leaked |
| `mirror` (pid 29916, console `--window` view) | — | (2740,1200) | still leaked |

Window position is not the mechanism. **Do not retest by moving windows.** The
doc's stated likely cause — the pointer reaching the RDP client thumbnail — is
eliminated, as is the console-side mirror panel.

Also eliminated earlier: `ClipCursor`, `WH_MOUSE_LL` (`cursorfence.exe`, which
is not even loaded — confirmed ABSENT in the process list).

What remains untested is the injection path itself. Catching this needs
injector-side logging of what arrives at `seatB_agent` at the moment a leak
happens. That is a build, not a test.

### Logs in `C:\ProgramData\Hydra\logs` are startup-only

Two hours were spent today reading stale files as live state. They are not.

- `capture_B.log` ends at `entering capture loop` and never writes again.
  Last write 1:51:17 PM while capture ran healthily for another five hours.
- `router.log` stopped at 1:53:28 PM. Its `kbd=0 mouse=0` lines and the dozen
  agent connect/disconnect cycles are **startup settling**, not live state —
  seat B's keyboard and mouse work fine.

**Check `LastWriteTime` before reading any of these as current.** And note that
`hydractl status` reports process liveness, not work done: a process can be
"running (up 16495s)" while doing nothing.

### PROBLEM 1 — evidence capture is now automatic

`hydra-blackbox.ps1` runs the `ON-LOCKUP.md` capture on its own.

- 5s samples into a 30-minute rolling buffer
- dumps to `logs\STALL-<timestamp>.txt` when `mirror` CPU goes flat for 24
  consecutive samples (2 minutes)
- records per-process CPU/WS/threads/handles, window `iconic`/`visible`/`rect`,
  `query session`, `hydractl status`
- **launch elevated** or the `hydractl` section reads `err 5` (ACCESS_DENIED —
  the control pipe is SYSTEM-owned; `err 2` would mean missing)
- `caplog` is permanently flat because of the startup-only logging above, so
  the stall signal is mirror's CPU alone. Hence 24 samples rather than 6.

Restart the seat freely when it dies. The evidence writes itself first.

### PROBLEM 2 — the binary is armed

`rdp\hydra_veh.c`, `#include`d from `hydrardp.c` just above `main()`, with
`hydra_install_veh()` immediately after `SetUnhandledExceptionFilter`. Single
translation unit, so `build-rdpclient.ps1` is unchanged.

On the null-pointer AV it writes `logs\gfx_crash.txt` containing `[rsp+00]`
resolved to module+RVA. The `call` pushed its return address before faulting,
so that value names the libfreerdp3 function that called the null pointer.
No hypothesis required — this replaces guess twelve.

Verify it is in the build:

```powershell
$b = [System.IO.File]::ReadAllBytes('C:\Programs\hydra\dist\hydrardp.exe')
[System.Text.Encoding]::ASCII.GetString($b).Contains('gfx_crash.txt')
```

Crash run still pending. Needs `sdl-freerdp` down and budgets a reboot.

One correction to the doc's "best remaining lead": `MapSurfaceToWindow` and
`MapSurfaceToScaledWindow` are RAIL-path PDUs. A server driving a plain desktop
session sends `MapSurfaceToOutput` instead, so those two should never fire.
The lead is probably wrong, but its shape generalises — any of the ~20 callback
slots `gdi_graphics_pipeline_init` populates produces the same signature if
left unset.

### Strategic note

Modes 1 and 2 capture with DDA inside the session; the RDP client is only a
session holder. Mode 3 is the only mode where the client *is* the capture path,
which is why it needs `/gfx`. Mode 3 exists solely to be immune to the
minimize/cover freeze — an mstsc pathology sdl-freerdp does not have.

So if PROBLEM 1 turns out not to be client-window suppression, mode 3 buys
nothing and PROBLEM 2 is dead work. Diagnose 1 before spending more on 2.

### Environment notes

- Elevated shell breaks `GetForegroundWindow` (UIPI), so the recorder's `fgnd`
  field reads `hwnd=0`. Per-process `MainWindowHandle` still works, which is the
  part that matters.
- Two `mirror` processes: pid 564 (seat panel, 3240,0–5160,1080) and pid 29916
  (console `--window` view, started 2 min later).
- `Get-CimInstance Win32_Process` returns blank `CommandLine` unelevated.
- `capture_B.log` shows five `attached to WinSta0 / interactive input desktop`
  retries before duplication succeeded. Unexplained, low priority.
