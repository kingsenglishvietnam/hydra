# EXCLUSIVE-FULLSCREEN.md — procedure and undo

**What this is for.** Aero Peek makes the seat panel briefly transparent when
you hover a taskbar thumbnail on the console. DWM applies Peek to *every*
top-level window, has no per-window exclusion, and cannot be disabled on
Windows 8+. A window outside composition entirely is the only thing Peek does
not reach — and exclusive fullscreen is how you get one.

**Scope: mode 6 only.** Mode 7 has no `mirror`; its FreeRDP client owns the
panel, and FreeRDP's SDL backend offers only borderless fullscreen (`+f`), with
no exclusive option. Fixing mode 7 the same way would mean forking FreeRDP.

**Status as of 2026-08-24:** `SetFullscreenState` returns `S_OK` — Windows
grants it. `Present` then fails with `DXGI_ERROR_INVALID_CALL` (`0x887A0001`)
until the buffers are resized to match the new mode. This procedure adds that
resize. Whether it actually stops Peek is **still untested**, because nothing
rendered before the fix.

**Opt-in.** Everything here is gated on `HYDRA_EXCLUSIVE=1`. Without it the
default path is byte-for-byte unchanged, so the launchers and modes 1, 2, 3 and
6 behave exactly as before.

---

## Why opt-in matters

Exclusive fullscreen **owns the display**. If the transition goes wrong the
panel is gone until the process exits. Worse, `mirror` is used by four modes and
is not code you want fragile.

There is also a design tension specific to this setup: crossing the console
cursor onto DISPLAY2 to reach the seat may pull `mirror` out of exclusive mode
repeatedly. If that happens the handling has to tolerate it, not fight it.

---

## Before you start

```powershell
cd C:\Programs\hydra
Copy-Item mirror\mirror.cpp "mirror\mirror.cpp.bak-$(Get-Date -f yyyyMMdd-HHmmss)"
```

Verify the line numbers — **this patch is line-indexed and will corrupt the file
if they have moved**:

```powershell
Get-Content mirror\mirror.cpp | Select-Object -Skip 468 -First 10 | ForEach-Object -Begin {$i=469} -Process { "$i : $_"; $i++ }
```

Required:

```
471 :         sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;   /* flip model = low latency */
472 :         sc.Scaling     = DXGI_SCALING_STRETCH;
473 :
474 :         hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sc, nullptr, nullptr, &swap);
475 :         return SUCCEEDED(hr);
```

If they differ, find the swapchain creation and adjust the indices:

```powershell
Select-String -Path mirror\mirror.cpp -Pattern 'CreateSwapChainForHwnd' | Select-Object LineNumber
```

`RemoveRange(N, 5)` where N is the zero-based index of the `sc.SwapEffect` line —
i.e. its 1-based line number minus one.

---

## Step 1 — patch the swapchain creation

```powershell
$f='C:\Programs\hydra\mirror\mirror.cpp'; $l=[System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($f)); $l.RemoveRange(470, 5); $l.InsertRange(470, [string[]]@(
'        sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;   /* flip model = low latency */',
'        sc.Scaling     = DXGI_SCALING_STRETCH;',
'',
'        /* EXCLUSIVE FULLSCREEN, opt-in with HYDRA_EXCLUSIVE=1.',
'         *',
'         * Aero Peek makes every top-level window transparent on a taskbar hover,',
'         * including this one, so the seat panel briefly shows the desktop behind',
'         * it. DWM has no per-window exclusion and cannot be disabled. A window',
'         * outside composition entirely is the only thing Peek does not reach.',
'         *',
'         * ALLOW_MODE_SWITCH is required for SetFullscreenState, and every later',
'         * ResizeBuffers MUST pass the same flags or it fails.',
'         *',
'         * Opt-in because exclusive mode OWNS the display: a bad transition loses',
'         * the panel until this process exits. Default path unchanged. */',
'        const bool wantExclusive = (_wgetenv(L"HYDRA_EXCLUSIVE") != nullptr);',
'        if (wantExclusive) sc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;',
'',
'        hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sc, nullptr, nullptr, &swap);',
'        if (FAILED(hr)) return false;',
'',
'        if (wantExclusive) {',
'            HRESULT fsHr = swap->SetFullscreenState(TRUE, nullptr);',
'            fwprintf(stderr, L"[mirror] SetFullscreenState -> 0x%08lX\n", (unsigned long)fsHr);',
'            if (SUCCEEDED(fsHr)) {',
'                /* MANDATORY. Present returns DXGI_ERROR_INVALID_CALL (0x887A0001)',
'                 * until the buffers match the new mode. Zeros = adopt current. */',
'                HRESULT rb = swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,',
'                                                 DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);',
'                fwprintf(stderr, L"[mirror] post-fullscreen ResizeBuffers -> 0x%08lX\n",',
'                         (unsigned long)rb);',
'            }',
'            fflush(stderr);',
'        }',
'',
'        return true;'
)); [IO.File]::WriteAllLines($f,$l); Get-Content $f | Select-Object -Skip 468 -First 42 | ForEach-Object -Begin {$i=469} -Process { "$i : $_"; $i++ }
```

**GATE:** the printed block must end `return true;` then `}` then `};`. If it
does not, undo (below) and stop.

---

## Step 2 — the other two ResizeBuffers sites

**Not yet done.** `ResizeBuffers` fails if the flags do not match those the
swapchain was created with, so both existing calls need the flag when exclusive
mode is on.

```powershell
Select-String -Path C:\Programs\hydra\mirror\mirror.cpp -Pattern 'ResizeBuffers' | Select-Object LineNumber, Line
```

Originally at 648 and 795 (they will have moved). Both pass `0` as the final
argument and need `wantExclusive ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0` —
which means the flag has to be reachable from those scopes, probably as a
`Gfx` member rather than a local.

Do this only if step 1 works.

---

## Step 3 — teardown and focus handling

**Not yet done.** Needed before this is usable day to day:

- **`SetFullscreenState(FALSE, nullptr)` before releasing the swapchain**, or
  Windows can leave the display in the wrong mode after `mirror` exits.
- **`WM_ACTIVATEAPP`** — DXGI drops exclusive mode when focus is lost. Decide
  whether to reclaim it or accept borderless as a fallback. Reclaiming may fight
  the cursor crossing onto the panel.
- **`DXGI_ERROR_INVALID_CALL` in the `Present` handler** (around line 703, the
  `else if (FAILED(ph))` branch) should trigger a resize rather than just being
  logged.
- **Never call `SetFullscreenState` from inside a window procedure** — it
  deadlocks.

---

## Build and deploy

```powershell
cd C:\Programs\hydra
.\hydra7.ps1 -Stop
Get-Process mirror, seat_router, seatB_agent, audio_bridge, hydrad, session_capture -EA SilentlyContinue | Stop-Process -Force
Start-Sleep 2
.\build.ps1
```

**No `LNK1104`.** If you see it, something is still holding the exes — stop
everything and retry. A partial build leaves a stale `dist\mirror.exe` and you
will spend an hour debugging code that is not running.

```powershell
Get-Item dist\mirror.exe | Select-Object Length, LastWriteTime
```

Timestamp must be from this build.

```powershell
(Get-Content seats.toml -Raw) -replace '(?m)^display_mode = ".*"', 'display_mode = "capture"' | Set-Content seats.toml -NoNewline
.\setup.ps1
Select-String -Path dist\seats.toml -Pattern '^display_mode'
```

Must read `capture`. Mode 6 needs it; mode 7 leaves it `off`.

---

## Test

**A live seat session is required.** Without one there is no capture, no ring,
and `mirror` exits silently — which cost an hour of confusion on 2026-08-24.

```powershell
.\hydra6.ps1
```

Log in as `teacher`, wait for the seat to appear on the panel.

```powershell
.\dist\hydractl.exe status
```

`capture:B: running`, **not** `waiting (session user:teacher)`.

Then replace the launcher's mirror with the exclusive one:

```powershell
Get-Process mirror -EA SilentlyContinue | Stop-Process -Force
$env:HYDRA_EXCLUSIVE=1
.\dist\mirror.exe B \\.\DISPLAY2
```

Run it in the **foreground** so its output is visible.

### What to look for

```
[mirror] SetFullscreenState -> 0x00000000
[mirror] post-fullscreen ResizeBuffers -> 0x00000000
[mirror B] presenting on \\.\DISPLAY2 (1920x1080), ...
```

| result | meaning |
|---|---|
| both `0x00000000`, seat visible | works — hover a taskbar thumbnail and see if the panel stays solid |
| `Present failed hr=0x887A0001` | the resize did not take. Step 1 is wrong. |
| `SetFullscreenState` non-zero | Windows refused. Not achievable; undo. |
| panel black or blank | exclusive mode took the display. **Ctrl+C in that window immediately.** |

**Exit with Ctrl+C in the mirror's own window.** It holds the display mode;
killing it from elsewhere can leave the monitor in the wrong state.

---

## UNDO

### Just stop using it

```powershell
Get-Process mirror -EA SilentlyContinue | Stop-Process -Force
Remove-Item Env:\HYDRA_EXCLUSIVE -EA SilentlyContinue
```

The code is opt-in, so without the variable everything behaves as before. This
is enough for day-to-day use.

### Remove the code entirely

```powershell
cd C:\Programs\hydra
Get-Process mirror -EA SilentlyContinue | Stop-Process -Force
Copy-Item (Get-ChildItem mirror\mirror.cpp.bak-* | Sort-Object LastWriteTime -Desc | Select-Object -First 1).FullName mirror\mirror.cpp -Force
Select-String -Path mirror\mirror.cpp -Pattern 'SetFullscreenState|HYDRA_EXCLUSIVE' | Select-Object LineNumber
```

Silence means it is out.

```powershell
.\hydra7.ps1 -Stop
Get-Process mirror, seat_router, seatB_agent, audio_bridge, hydrad, session_capture -EA SilentlyContinue | Stop-Process -Force
Start-Sleep 2
.\build.ps1
.\setup.ps1
```

Or from git, if the backup is missing:

```powershell
git checkout mirror/mirror.cpp
```

### If the panel is stuck in a bad mode

```powershell
Get-Process mirror -EA SilentlyContinue | Stop-Process -Force
```

Then reset the display: **Win+P**, select **Extend**. Or unplug and replug the
monitor. A reboot always clears it.

### Back to a working seat

```powershell
cd C:\Programs\hydra
(Get-Content seats.toml -Raw) -replace '(?m)^display_mode = ".*"', 'display_mode = "off"' | Set-Content seats.toml -NoNewline
.\setup.ps1
.\hydra7.ps1
```

---

## The alternative, if this proves not worth it

A long Peek hover delay keeps the feature and stops accidental triggering. Works
in **both** modes, needs no code, and takes one command:

```powershell
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name DesktopLivePreviewHoverTime -Value 5000 -Type DWord
Stop-Process -Name explorer -Force
```

Five seconds of deliberate hover before Peek fires.

Or disable Peek entirely:

```powershell
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name DisablePreviewDesktop -Value 1 -Type DWord
Stop-Process -Name explorer -Force
```

---

## Lessons from the first attempt, 2026-08-24

**Check `hydractl status` before debugging `mirror`.** An hour went into
"`mirror` prints nothing" that was entirely `capture:B: waiting` — no seat
session, no ring, so `mirror` had nothing to attach to and exited. Nothing to do
with the code change.

**The insert landed before `CreateSwapChainForHwnd` the first time**, so `swap`
was still null and the block never ran. Always print the surrounding lines with
numbers afterwards and read them.

**`.Replace` on a here-string fails silently against a CRLF file** when the
here-string is LF. Several edits that day did nothing and looked like they had
worked. Use `ReadAllLines`/`WriteAllLines` with indices, and cast inserted arrays
to `[string[]]` or `InsertRange` rejects them.

**A partial build leaves a stale exe.** `LNK1104` on some files still produces a
"successful"-looking run. Check the timestamp.
