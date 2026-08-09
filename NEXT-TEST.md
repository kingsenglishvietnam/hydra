# Next test — gfx fix.  Run these first thing after a reboot.

The fix is BUILT but UNTESTED: the run after it hit a wedged stack.

**What changed:** our special case for the gfx channel was removed. FreeRDP's own
SDL client (3.30, client/SDL/SDL2/sdl_channels.cpp) handles RAIL, CLIPRDR and
DISP and passes EVERYTHING ELSE to `freerdp_client_OnChannelConnectedEventHandler`
-- it never mentions RDPGFX and never calls `gdi_graphics_pipeline_init`.
Intercepting gfx skipped the common handler's setup, and something later called
through the pointer it never set. That is the crash:
`libfreerdp-client3 -> libfreerdp3 -> null` on a channel thread.

---

## 1.  FIRST THING after boot.  Nothing else touches the session before this.

```powershell
cd C:\Programs\hydra; Start-Service Hydra; Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue; $env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher
```

**Expect:**
- NO `graphics pipeline attached` line -- that was ours and is gone. Its absence
  is correct.
- NO `FATAL: exception 0xC0000005`
- `pixel transport opened`, then publishes climbing

**If `ERRCONNECT_ACTIVATION_TIMEOUT`:** stack still wedged. Reboot, retry. The
test has not run.

**If it crashes again:** stop. Do not patch. Six hypotheses failed before reading
the source; the next step is diffing the rest of the init path against
`client/common/client.c`, not a seventh guess.

---

## 2.  Mirrors — second shell, only once publishes are climbing

```powershell
cd C:\Programs\hydra; Get-Process mirror -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','\\.\DISPLAY2' -WindowStyle Minimized; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','--window','1600x900','56789'
```

```powershell
Get-Process mirror | Select-Object Id, MainWindowTitle, WorkingSet
```

Two processes, 70-98 MB each. Under 20 MB = started too early, rerun.

---

## 3.  Judge it

Play a video in the seat's session. Compare against the bitmap path:

```powershell
Get-Process hydrardp | Stop-Process -Force
Remove-Item Env:HYDRA_GFX
.\dist\hydrardp.exe B teacher
```

RFX should be visibly better. If it is not, gfx is not worth keeping.

Also try:

```powershell
$env:HYDRA_GFX='progressive'
```

**Do not use `HYDRA_GFX=1`** -- that lets the server pick H.264, and this
libfreerdp is built `WITH_VAAPI_H264_ENCODING=ON`, which the library itself
warns is experimental.

---

## 4.  If it works

```powershell
git add -A; git commit -m "gfx: let the graphics channel fall through to the common handler, as the stock clients do"; git push
```

Then set `HYDRA_GFX=RFX` permanently:

```powershell
[Environment]::SetEnvironmentVariable('HYDRA_GFX','RFX','User')
```

---

## Fallback: a working system, one command

```powershell
cd C:\Programs\hydra; .\hydra-start.ps1
```

Mode 2, sdl-freerdp, DDA capture. Pixel-perfect video. Use this to teach.
