# Next session — start here

Built and ready to test: **clean shutdown** and **milestone 3 (the cursor)**.
Neither has been run yet.

---

## 1. Start mode 3

Elevated **x64 Native Tools Command Prompt** → `powershell` → then:

```powershell
cd C:\Programs\hydra
Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process -Force
Remove-Item Env:HYDRA_GFX -ErrorAction SilentlyContinue   # gfx still crashes; leave it off
Start-Service Hydra
Stop-Process -Name session_capture -Force -ErrorAction SilentlyContinue
.\dist\hydrardp.exe B teacher
```

Leave that shell in the foreground. Look for:

```
connected; GDI ready (pointer handled by us, not drawn into the buffer)
pixel transport opened -- mirror will display these frames
seat B: N paints, N published, N coalesced, N rows copied
```

Second shell:

```powershell
cd C:\Programs\hydra
Start-Process 'C:\Programs\hydra\dist\mirror.exe' -ArgumentList 'B','\\.\DISPLAY2' -WindowStyle Minimized
Start-Process 'C:\Programs\hydra\dist\mirror.exe' -ArgumentList 'B','--window','1600x900','56789'
```

---

## 2. The two things to judge

**Cursor.** Is it steady, or still flickering? We now take the pointer PDUs
ourselves and composite once per publish inside the seqlock, instead of sampling
a buffer FreeRDP draws and erases the cursor in.

*If it is missing entirely* — the pointer callbacks registered but nothing is
arriving, or the compositing offsets are wrong.
*If it lags behind the real position* — position updates arrive but we only
composite on publish, so it moves at the publish rate.

**Clean shutdown — test this deliberately, it matters more than the cursor.**

```powershell
# Ctrl+C the client, then:
.\dist\hydrardp.exe B teacher
```

It must reconnect **without a reboot**. Watch for `disconnecting cleanly`.

Then the harder case:

```powershell
Get-Process hydrardp | Stop-Process -Force
.\dist\hydrardp.exe B teacher
```

A killed process is what wedged the stack four times in one evening: the wrapper
keeps a session it cannot clean up and every later connection dies at
`ERRCONNECT_ACTIVATION_TIMEOUT`. If reconnecting works now, the client is
finally safe to leave running.

---

## 3. If the stack wedges anyway

`ERRCONNECT_ACTIVATION_TIMEOUT` on connect = wedged. Confirm it is not our code
by running the stock client with the same target:

```powershell
C:\msys64\mingw64\bin\sdl-freerdp.exe /v:127.0.0.2 /u:teacher /cert:ignore
```

Failing identically means the wrapper, not us. Reboot; nothing else clears it.

---

## 4. If you need a working system instead

Mode 2 is the one to teach on, and it is one command:

```powershell
.\hydra-start.ps1
```

---

## Then, in order

1. **Clean shutdown confirmed** — everything else depends on it.
2. **Milestone 4, audio.** Fold audio into the client's own stream. Fixes the
   drift between picture and sound under load, which happens because
   `audio_bridge` and the client are independent paths with independent buffers.
3. **The graphics pipeline.** `/gfx` crashes immediately after
   `graphics pipeline attached`. It needs more of FreeRDP's client-common
   scaffolding than has been reconstructed so far — read `sdl-freerdp`'s source
   rather than guessing again. This is what makes video stop looking blocky.
4. **Milestone 5, input.** Input still goes the long way round through
   Interception and the agent.
