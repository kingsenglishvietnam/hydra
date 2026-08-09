# Hydra — current state.  Read this first after any reboot.

Written because this conversation gets COMPACTED: earlier hours become a summary
and details are lost. The file on your disk is the record; my memory is not.
Update it as things change.

**When in doubt, read the source, not this file:**

```powershell
Select-String -Path .\rdp\hydrardp.c -Pattern "<thing>" -Context 0,10
```

---

## Three modes

| Mode | Command | State |
|---|---|---|
| mstsc | `.\hydra-start.ps1 -Client mstsc` | Works. Panel freezes if the client window is minimized or covered. |
| sdl-freerdp | `.\hydra-start.ps1` | **Teach on this.** Client does not suppress output, so no freeze. |
| hydrardp | see below | Development. Headless client, no window at all. |

**Never run two modes at once.** Two clients on one session wedges the RDP stack,
and a wedged stack needs a REBOOT.

---

## hydrardp — what is built

- Connects headless, publishes frames into `Global\HydraSeat_B_pix`
- 60 fps throttle; full-frame copies (a damage-rect version was tried and
  removed -- it fought the cursor compositing)
- Pointer taken over from FreeRDP and composited by us, inside the seqlock
- Cursor POSITION comes from `agent:B` via `pixHdr->curSeq` -- RDP does not send
  positions to a client that generates no input
- Crash handler (`SetUnhandledExceptionFilter`) disconnects before dying
- Requests a full refresh after connect, so an idle desktop is not blank
- gfx callbacks present: `hydra_gfx_map_window`, `hydra_gfx_unmap_window`,
  `hydra_gfx_update_surface_area`

## HYDRA_GFX — selects the CODEC, not on/off

| Value | Effect |
|---|---|
| `1` | `/gfx` -- server picks, usually **H.264. CRASHES.** |
| `RFX` | `/gfx:RFX` -- RemoteFX, no H.264 |
| `progressive` | `/gfx:progressive` |
| anything else | passed through (`AVC420` etc.) |

**Why `1` crashes:** this libfreerdp is built `WITH_VAAPI_H264_ENCODING=ON`,
which the library itself warns is experimental and "might crash the
application". The fault is a call through a null pointer immediately after the
pipeline attaches -- a half-initialised codec. FreeRDP issue 12221 is the same
shape. Avoiding H.264 avoids the path.

---

## Startup, mode 3.  ORDER MATTERS.

Service first (it creates the ring), then client, then mirrors. A mirror started
against an empty ring sits at ~7 MB showing nothing.

**Shell 1:**

```powershell
cd C:\Programs\hydra; Start-Service Hydra; Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue; Remove-Item Env:HYDRA_GFX -ErrorAction SilentlyContinue; .\dist\hydrardp.exe B teacher
```

Wait for `pixel transport opened` AND publishes climbing.

**Shell 2:**

```powershell
cd C:\Programs\hydra; Get-Process mirror -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','\\.\DISPLAY2' -WindowStyle Minimized; Start-Process '.\dist\mirror.exe' -ArgumentList 'B','--window','1600x900','56789'
```

**Check:** `Get-Process mirror | Select-Object Id, MainWindowTitle, WorkingSet`
-- two processes, 70-98 MB each.

**Full stop:**

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc -ErrorAction SilentlyContinue | Stop-Process -Force; Stop-Service Hydra
```

then `query session` and `logoff <teacher's ID>`.

---

## Symptoms

| Symptom | Cause |
|---|---|
| `ERRCONNECT_ACTIVATION_TIMEOUT` | Stack wedged. **Reboot.** Confirm it is not us: `C:\msys64\mingw64\bin\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /cert:ignore` failing the same way means the wrapper. |
| `ERRCONNECT_LOGON_FAILURE` | Wrong password. Echo is off, so a typo is invisible. |
| `no pixel ring` | Service not running. `Start-Service Hydra` |
| Mirrors ~7 MB, blank | Started before the client was publishing. |
| `LNK1104` on build | The exe is running. Kill it first. |
| Video glitchy, no gfx | Expected -- plain bitmap updates, no codec. |
| Cursor absent | Needs `agent:B` publishing positions; check the service is up. |

---

## Do not

- Extract a whole-tree zip over the working tree. One stale zip downgraded the
  sources and cost a `git checkout -- .` recovery. **Individual files only.**
- Use `HYDRA_GFX=1`. Crashes -- see above.
- Test clean shutdown with `Stop-Process -Force`. That is `TerminateProcess` and
  no user-mode handler can intercept it. Use **Ctrl+C**.
- Run two modes at once.

---

## Open

1. gfx with `RFX` / `progressive` -- untested on a clean stack.
2. Video quality without a codec -- expected to be poor.
3. Milestone 4 (audio into the client's own stream), milestone 5 (input).

## gfx: six hypotheses eliminated

Crash is ALWAYS: exception 0xC0000005 at address 0, on a channel thread, immediately after 'graphics pipeline attached'. gdb backtrace shows libfreerdp-client3 -> libfreerdp3 -> null, with NO hydra_* frames.

Ruled out: the codec (RFX and AVC fail identically); null map-window callbacks (init_ex with stubs is in place); double channel init; our pointer registration (bisect confirmed skipped, still crashes); chaining GDI's EndPaint; not chaining it.

KEY FACT: sdl-freerdp.exe /v:127.0.0.2 /u:teacher /cert:ignore /gfx:RFX WORKS on this machine with this library. So it is our initialisation, and the difference is findable by DIFFING against client/SDL/sdl_freerdp.cpp and client/common/client.c from the FreeRDP source -- not by more hypotheses. mingw-w64-x86_64-freerdp-debug does not exist, so gdb cannot name the function.

NEXT: read the source, diff the init path end to end. Do not patch on a hunch -- six have failed.

## gfx: the actual cause

FreeRDP 3.30 client/SDL/SDL2/sdl_channels.cpp handles RAIL, CLIPRDR and DISP, and passes EVERYTHING ELSE to freerdp_client_OnChannelConnectedEventHandler. It never mentions RDPGFX_DVC_CHANNEL_NAME and never calls gdi_graphics_pipeline_init. Our special case for gfx was the bug: intercepting the channel skipped the common handler's setup, and something later called through the pointer it never set -- libfreerdp-client3 -> libfreerdp3 -> null on a channel thread. Fixed by letting gfx fall through to the common handler (if (0) on both the connect and disconnect special cases). UNTESTED: the run after the fix hit a wedged stack.

## gfx: source-derived fix ALSO failed

Removed our RDPGFX special case so the channel falls through to freerdp_client_OnChannelConnectedEventHandler, matching client/SDL/SDL2/sdl_channels.cpp exactly. Confirmed applied ('graphics pipeline attached' no longer prints). STILL crashes identically. Seven attempts. The difference from the stock client is elsewhere in the init path -- diff settings, PreConnect and context setup against client/common/client.c. Do not test another candidate.

## gfx: STOPPED after nine attempts

Ruled out: codec (RFX==AVC), map-window callbacks (init with NULLs is what every client uses), double channel init, our pointer registration, chaining EndPaint, not chaining it, intercepting the gfx channel, context layout (fixed rdpContext -> rdpClientContext, correct but not the cause), SoftwareGdi (=1, gdi non-null).

UNTESTED, both structural: (1) we call freerdp_connect directly, SDL calls freerdp_client_start which connects on its own thread -- our crash is on a channel thread. (2) we replace update->EndPaint in PostConnect, and gdi_graphics_pipeline_init installs its own EndPaint later; no stock client overrides EndPaint at all.

DO NOT patch further without testing one of those two properly.

## NEXT (after reboot, first thing)

```powershell
cd C:\Programs\hydra; Start-Service Hydra; Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue; $env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher
```n
Built but untested: EndPaint is no longer overridden under gfx. Look for 'EndPaint NOT overridden (gfx)'. ZERO publishes is expected and correct. The only question is whether it still crashes. No crash = EndPaint was the cause. Crash = it is the freerdp_connect vs freerdp_client_start thread difference.

## gfx: STOPPED at ten attempts

Ruled out: codec (RFX==AVC identical), map-window callbacks (init with NULLs is what every stock client uses), double channel init, our pointer registration, intercepting the gfx channel, context layout (rdpContext -> rdpClientContext, a real bug, fixed, not the cause), SoftwareGdi (=1, gdi non-null).

Inconclusive: skipping the EndPaint override still produced 107 paints / 66 published, so BeginPaint or something else still drives our publish path. The isolation was not clean.

UNTESTED and structural: we call freerdp_connect on the main thread; SDL calls freerdp_client_start, which connects on its own thread via ClientStart. Our ClientStart is a stub. The crash is on a channel thread, so this is the strongest remaining candidate -- and testing it means restructuring the client, not a one-line patch.

DO NOT patch further. Mode 3 works without gfx. Mode 2 is pixel-perfect for teaching.

## gfx: STOPPED at ten eliminated candidates

Thread affinity ruled out -- crashes identically on a spawned connect thread (visible as a different thread id in the log). Also ruled out: codec, map-window callbacks, double channel init, pointer registration, intercepting the gfx channel, context layout, SoftwareGdi, chaining EndPaint, not overriding EndPaint.

Remaining territory: the SURFACE path. gfx delivers content as surfaces blitted on its own schedule, not into primary_buffer via EndPaint. Even without the crash the publish path needs rework for that. Read libfreerdp/gdi/gfx.c before touching anything.

PRIORITY IS NOW MODE 2 LOCKUPS -- that is what teaching depends on. Use ON-LOCKUP.md before restarting.
