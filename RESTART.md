# Hydra — restart cheat sheet

Keep this open. Every sequence is ONE LINE so a paste cannot be reordered.

---

## Mode 2 — sdl-freerdp.  USE THIS TO TEACH.

```powershell
cd C:\Programs\hydra; .\hydra-start.ps1
```

Optional view window on the console screen:

```powershell
cd C:\Programs\hydra; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','--window','1600x900','56789'
```

---

## Mode 3 — hydrardp.  DEVELOPMENT ONLY.

**Shell 1 — service + client** (elevated x64 Native Tools -> `powershell`):

```powershell
cd C:\Programs\hydra; Set-ExecutionPolicy Bypass -Scope Process -Force; Remove-Item Env:HYDRA_GFX -ErrorAction SilentlyContinue; Start-Service Hydra; Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue; .\dist\hydrardp.exe B teacher
```

Wait for `pixel transport opened` AND publishes climbing before starting mirrors.
Starting them against an empty ring leaves them at ~7 MB showing nothing.

**Shell 2 — mirrors** (plain PowerShell as admin is fine):

```powershell
cd C:\Programs\hydra; Get-Process mirror -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','\\.\DISPLAY2' -WindowStyle Minimized; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','--window','1600x900','56789'
```

**Check:**

```powershell
Get-Process mirror | Select-Object Id, MainWindowTitle, WorkingSet
```

Two processes, 70-98 MB each. Under 20 MB = started too early, rerun shell 2.

---

## Build

```powershell
cd C:\Programs\hydra; Get-Process hydrardp -ErrorAction SilentlyContinue | Stop-Process -Force; .\build-rdpclient.ps1
```

Everything else (mirror, agent, router, hydrad):

```powershell
cd C:\Programs\hydra; Get-Process mirror, hydrardp -ErrorAction SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; .\build.ps1; .\setup.ps1
```

Expect **Built (14)**. If it says 7, something has overwritten your sources --
recover with `git checkout -- .`

---

## Full stop

```powershell
Get-Process mirror, hydrardp -ErrorAction SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; Get-Process sdl-freerdp, mstsc -ErrorAction SilentlyContinue | Stop-Process -Force
```

Then clear the seat session:

```powershell
query session
logoff <teacher's ID>
```

---

## When it goes wrong

| Symptom | Do this |
|---|---|
| `ERRCONNECT_ACTIVATION_TIMEOUT` | Stack wedged. Reboot -- nothing else clears it. Confirm it is not our code first: `C:\msys64\mingw64\bin\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /cert:ignore` failing the same way means the wrapper. |
| `ERRCONNECT_LOGON_FAILURE` | Wrong password. The prompt has echo off, so a typo is invisible. Just rerun. |
| `LNK1104` / `Permission denied` on a build | The exe is running. Kill it first (see Build above). |
| Mirrors blank, ~7 MB | Started before the client was publishing. Rerun shell 2. |
| Panel frozen, mode 2 only | The RDP client window is minimized or covered. It must stay visible. |
| Video glitchy in mode 3 | Known. No video codec -- `/gfx` still crashes. Mode 2 captures the real desktop and is pixel-perfect. |
| Cursor does not track in mode 3 | Known. RDP does not send pointer positions to a client that sends no input. |

---

## Never

- Run two modes at once. Two producers on one ring, or two clients on one
  session, wedges the RDP stack.
- Extract a whole-tree zip over a working tree. One stale zip downgraded the
  sources and cost a Git recovery. Individual files only.
- Set `HYDRA_GFX=1`. It crashes at `graphics pipeline attached`, before any
  frame. The crash handler now disconnects cleanly, but there is no reason to
  invite it.
