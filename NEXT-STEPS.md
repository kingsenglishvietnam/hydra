# Hydra — next steps, from `gfx-working` (2026-08-11)

PROBLEM 2 is closed. Three faults, all in the `/gfx` path, all fixed and
confirmed by a student on seat B: `update->DesktopResize` NULL was the crash,
the late throttle check was the CPU, and blind-timer sampling was the glitching.

What follows is in the order that gets the most back for the least effort.

---

## 1. Run mode 3 for a real lesson

Everything below is speculative until mode 3 has carried actual teaching. It has
only ever been run in short test bursts.

Two windows, `hydrardp` first:

```powershell
cd C:\Programs\hydra; Start-Service Hydra; $env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher
```

```powershell
cd C:\Programs\hydra; .\dist\mirror.exe B \\.\DISPLAY2
```

Do **not** run `hydra-start.ps1` for this — it launches `sdl-freerdp`, and two
clients on one session wedges the stack.

What to watch for over a full session: does the freeze that mode 3 exists to
prevent actually stay away, does audio stay in sync, does the panel survive
seat B locking and unlocking.

If it holds up for a week, mode 3 becomes the default and `hydra-start.ps1`
needs a `-Client hydrardp` path so it is one command like the others.

---

## 2. PROBLEM 5 — the reboot tax

Three reboots today, roughly a dozen over the project. Every one came from
`hydrardp` dying and leaving the wrapper holding a session that only a reboot
clears. This is now the most expensive problem left.

`SetUnhandledExceptionFilter` handles clean crashes and has been seen working,
but `TerminateProcess` cannot be intercepted from inside the process, so the
hole cannot be closed in-process.

**Build a supervisor.** A parent that launches `hydrardp` with
`Start-Process -PassThru`, waits on the handle, and on *any* exit — clean,
crashed or killed — immediately runs `logoff <sessionid>` before anything else
tries to connect. Perhaps forty lines of PowerShell.

Also retire `Stop-Process -Force` from muscle memory: give `hydractl` a `stop`
verb that signals a named event, so the graceful path is the easy one.

---

## 3. PROBLEM 4 — the cursor leak, ~2 minutes

Diagnosed and not yet done. The seat monitor `\\.\DISPLAY2` sits adjacent to the
console monitor's right edge in the Windows arrangement, so the pointer simply
walks onto it. Not input forwarding, not a window — three reposition tests all
still leaked.

Open Settings → System → Display. Drag the seat monitor **above** the console
monitor, nudged sideways so only a couple of hundred pixels of the edges touch.
Windows only lets the pointer cross where edges overlap, so the crossing zone
shrinks to a corner you never visit.

Above rather than left: vertical overshoot is rarer, and the top edge is already
guarded by title bars and tab strips.

Afterwards, delete `cursorfence.exe` and the `ClipCursor` notes — both were
attacking a mechanism that does not exist.

---

## 4. PROBLEM 1 — still never diagnosed

`hydra-blackbox.ps1` has been running on and off since yesterday and has caught
nothing, because the machine has been rebooted more often than it has locked up.

Relaunch it after every reboot and leave it alone. Elevated window:

```powershell
cd C:\Programs\hydra; .\hydra-blackbox.ps1 -StallSamples 24
```

**Improve the signal while you are there.** It currently watches `mirror`'s CPU
delta, because `capture_B.log` turned out to be startup-only. The real signal is
`HydraSeatPixels.seq` in shared memory, which `hydra-shm.ps1` already reads —
`seq` flat while the seat is in use is a stall, definitively, no inference.
Folding that reader into the recorder makes the detector exact rather than
heuristic.

Also fold in: a mutex so a second instance refuses to start, and retry-on-lock
instead of dying. Two recorders fighting over one log file cost twenty minutes
today.

Worth checking too: `HydraSeatMeta.stalled` is a retry counter the capture side
already publishes for "attached to the desktop but EnumOutputs returns nothing"
— exactly PROBLEM 1's first candidate, instrumented long ago and never read.
The recorder should alarm on it.

---

## 5. PROBLEM 3 — verify, then close

`agent:B` publishes cursor position fine (`curSeq` climbing ~62/sec), and
`pointer IMAGE received` now appears under gfx. What has never been confirmed is
the client reading it end to end.

Run mode 3, move seat B's mouse, and watch whether the composited cursor tracks
on the panel. If it does, this is finished. If not, `hydra_composite_pointer`
is reading `curX/curY` but the seat's own log line — "cursor: image but no
position yet" — will say so.

---

## 6. Loose ends worth an hour each

**Argument parsing.** `hydrardp <seat> <user> [password] [host]` silently treats
a stray fifth argument as nothing and a stray third as the password. Passing
`/gfx:RFX` positionally sent it as teacher's password and produced an
indistinguishable `ERRCONNECT_LOGON_FAILURE`. Make it reject what it does not
recognise.

**Line endings.** `hydrardp.c` is now mixed — 1048 CRLF against 44 bare LF from
the patch scripts. Normalise to CRLF and add a `.gitattributes` before it
confuses a future diff.

**The stale-exe trap.** A running `hydrardp` locks `dist\hydrardp.exe`; the
compile then succeeds while the link fails with `Permission denied`, leaving the
old binary in place. `build-rdpclient.ps1` should kill any running instance
first and print the exe timestamp after.

**Dead scripts.** `fix-gfx-signal.ps1` did not work (the surface-area callback
never fires on this server) and `fix-throttle-early.ps1` had the line-ending
bug. Both are superseded. `hydra-display.ps1` has broken enumeration. Delete or
mark them.

---

## 7. Only if mode 3 disappoints — VNC

`mingw-w64-x86_64-libvncserver` is available in MSYS2 and the in-session
question has an answer: run `winvnc.exe` as an app inside the seat's session via
`seatB_agent`, not as a service.

But the reason for considering it was the `/gfx` crash, and that is fixed. VNC
would also inherit the same no-display-no-frames coupling that DDA has, so it
answers a question that is no longer being asked. Revisit only if mode 3 fails
in a way that capture-path replacement would fix.

---

## Traps, so they are not paid for a third time

- The gfx switch is the **`HYDRA_GFX` env var**, never a command-line flag.
- Kill `hydrardp` before building, and check the exe timestamp after.
- Logs under `C:\ProgramData\Hydra\logs` are **startup-only**. Check
  `LastWriteTime` before believing any of them. `hydra-shm.ps1` reads live state.
- `hydractl status` needs elevation or returns `err 5`.
- Never run two RDP clients against one session.
- Patch anchors must tolerate `\r?\n`, and the source must be read before
  patching — literal-string anchors have failed silently more than once.

## RDS protocol provider — first step done 2026-08-11

Azure/Remote-Desktop-Services-Protocol-Sample cloned to C:\Programs\rdsprov and BUILDS.

    msbuild TestProtocol_Ext.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.28000.0 /p:SpectreMitigation=Spectre

SpectreMitigation is required: only the spectre ATL libs are installed, there is no plain atlmfc\lib\x64\atls.lib.

Key findings from reading it:
- IWRdsWddmIddProps is implemented on the CONNECTION object. GetHardwareId returns the hardware ID from your IDD's INF; the RD stack then loads that driver INTO the session. That is the junction, and it is why the cross-session D3D11 wall does not arise.
- GetVideoHandle is documented 'not required if using IDD' — the IDD *is* the display. No capture, no duplication, no client window.
- GetInputHandles (keyboard/mouse/beep) is a TODO with no sample implementation. That is the one gap without a reference.
- SessionArbitrationEnumeration takes bSingleSessionPerUserEnabled and E_NOTIMPL gives default arbitration, so the provider participates in that decision. Does not settle the SKU session cap.
- The whole sample is ~450 lines of which maybe 40 do anything. Skeleton with correct shape.

Still unknown: whether termsrv's patched session policy (RDP-Wrapper patches SingleUserOffset, DefPolicyOffset, SLPolicyInternal, SLPolicyOffset) applies to a custom provider's listener. termsrv IS the RCM that loads providers, so it should — inference, not fact.


### RDS provider — how a connection is actually made

TestProtocolAPI.cpp, ~15 lines. No socket, no protocol:

1. CComObject<CWRdsProtocolConnection>::CreateInstance
2. SetCredentials(domain, user, password)
3. ZeroMemory the WRDS_CONNECTION_SETTINGS, then set WRdsConnectionSettingLevel = LEVEL_1 and WRdsListenerSettingLevel = LEVEL_1 — connections FAIL if these are unset
4. pListenerCallback->OnConnected(pConnection, &settings, &connCallback)
5. connCallback->GetConnectionId, pConnection->SetConnectionCallback
6. connCallback->OnReady()

Windows then creates the session. The sample's trigger is polling for C:\TestProtocol\createconnection.txt every 5s; for Hydra it would be hydrad saying go.

StartListen spawns a thread and holds the callback (must AddRef, Release in StopListen). The .rgs only registers the COM class under HKCR\CLSID with ThreadingModel=Free — the RD service finds providers elsewhere and the sample does not script that part.

Builds under BOTH toolsets:
  2026 (v145): needs /p:SpectreMitigation=Spectre — only spectre ATL libs installed
  2022 BuildTools (v143): needs ATL component added, then NO spectre flag — only plain libs

WARNING: registering a provider makes termsrv load the DLL into its svchost at service start. A bad one takes Terminal Services down, which stops seat B. Reboot-and-unregister recovery. Not during teaching hours.

