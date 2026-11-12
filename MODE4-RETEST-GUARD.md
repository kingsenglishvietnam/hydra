# MODE4-RETEST-GUARD.md — is reason=18 the session cap?

Written 2026-11-12. Read against repo `99db530`.

## What this is testing

Mode 4 was closed on 08-20 on one sentence in `cfe20f4`: null handles from
`GetInputHandles` "is what produces PreDisconnect reason 18". `MODE4-PROVIDER.md`,
written 45 minutes earlier, calls the same thing "the likely cause". Nothing in
the repo shows it was measured.

There is a second variable in those runs that nobody isolated. Every reason=18
run was under **stock, unpatched termsrv**, because RDP Wrapper had to be out of
the way for the provider to load. A second interactive logon on a client SKU
with no concurrency patch is exactly the thing termsrv exists to refuse.

The repo already holds a hint that the patch matters to provider sessions:

| date | termsrv | IDD | what the provider session did |
|---|---|---|---|
| 08-11 | RDP Wrapper loaded (patched policy) | none | full desktop: `explorer`, `dwm`, `teacher` **Active**, Disc at ~30 s |
| 08-14 → 08-18 | stock | none | stalls at `LogonUI`, no `explorer`, reason=12 at ~33 s, warm profile or cold |
| 08-20 | stock | working | reason=18 at ~0.4 s |

The later stalls were put down to a cold profile, and warming the profile
changed nothing. The patch is the difference nobody tested.

`termsrv-guard` patches `termsrv.dll` in place, so `ServiceDll` stays stock and
the provider should load **with** the patch. That combination has never run.

**This runbook does not write a driver, and does not decide mode 4.** It answers
one question: with the cap lifted, does the session still die at 0.4 s?

Three runs, one variable each:

| run | termsrv | IDD | answers |
|---|---|---|---|
| **A** | patched | none | does the provider load under the guard, and is 08-11 reproducible? |
| **B** | patched | `iddseat-remote` | **the question** |
| C (only if B holds) | stock | `iddseat-remote` | does 18 come back when the patch is removed? |

About two hours, with two reboots.

---

## Rules. All of them were paid for.

- **A morning with no lessons that day.** Not an evening. `safety-gate.ps1` will
  say so itself after 22:00.
- **At the physical console.** Never over RDP: RDP is the thing being restarted.
- **Recovery stick in E: before you start**, and the Surface on mains power.
- **One boot-affecting change per reboot.** Staging the driver is one. It gets
  its own reboot, and the provider is not registered across it.
- **`safety-gate.ps1` before the provider and again before the driver.** Skipped
  once, on 08-20: blank screen.
- **Never leave the listener registered with a trigger file present.** The next
  boot fires a connection on its own.
- **If Windows asks for a reboot, reboot.** Anything read before it is noise.
- **Read timestamps.** The helper only prints events from its own run.
- **A failed gate ends the session.** Do the cleanup in §7 and stop. Do not fix
  forward.
- **This machine is now build 26300.** Every mode 4 result in the repo is from
  26100. A different answer today may be the build, not the patch; run A is
  there partly to catch that.

### The emergency undo — copy this onto your phone now

If TermService will not start, or the seat is gone, on the physical console in
an admin `cmd`:

```
reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
sc stop TermService
sc start TermService
```

That is the whole recovery for the provider. For the driver, the gate writes
`UNDO-OFFLINE.txt` to the stick; read it from WinRE with `type`.

If the **console session** gets bumped to the lock screen during a run, the
patch is not in effect. Log back in and go straight to §7.

---

## 0. Put the helper in place

Copy `mode4-guard-helpers.ps1` to `C:\Programs\hydra`. Then, elevated Hydra
Shell:

```powershell
Set-Location C:\Programs\hydra
Unblock-File .\mode4-guard-helpers.ps1
. .\mode4-guard-helpers.ps1
```

It defines functions and runs nothing. **Dot-source it again after every
reboot.**

---

## 1. Preflight — read-only, changes nothing

### 1a. The patch is in effect

```powershell
.\tools\termsrv-guard\hydra-termsrv-guard.ps1 -DryRun
Test-Path .\state\termsrv-UNPATCHED
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters').ServiceDll
```

**GATE:** the guard says `OK: already patched`, the flag test says `False`, and
`ServiceDll` ends in `\System32\termsrv.dll`.

`DryRun: would archive original and patch` means it is **not** patched. Stop:
fixing that is its own job, and mode 7 is broken too.

### 1b. Test-signing is still on

```powershell
bcdedit /enum "{current}" | Select-String 'testsigning'
Confirm-SecureBootUEFI
```

**GATE:** `testsigning Yes`, and `False`.

### 1c. The provider DLL is the instrumented one

```powershell
$dll = 'C:\Programs\rdsprov\Sample\x64\Release\TestProtocol_Ext.dll'
$src = 'C:\Programs\rdsprov\Sample\TestProtocol_Ext\WRdsProtocolConnection.cpp'
Get-Item $dll, $src | Select-Object Name, Length, LastWriteTime
(Select-String -Path $src -Pattern 'HydraProvLog').Count
(Select-String -Path $src -Pattern 'HydraSeat_RemoteIDD_v1').Count
.\rdsprov-register.ps1 -Status
```

**GATE:** both files exist; the DLL is **newer** than the `.cpp`; the first
count is well above zero and the second is 1; the only listeners are `Console`
and `RDP-Tcp`.

A `HydraProto` listener already present means a previous session was not
cleaned up. Do §7 first, reboot, start again.

### 1d. The driver package is on disk and signed

```powershell
Get-ChildItem .\dist\driver-remote | Select-Object Name, Length, LastWriteTime
Get-ChildItem .\dist\driver-remote\*.cat |
    Get-AuthenticodeSignature |
    Select-Object Status, `
        @{ n = 'Signer';  e = { $_.SignerCertificate.Subject } }, `
        @{ n = 'Expires'; e = { $_.SignerCertificate.NotAfter } }
Select-String -Path .\dist\driver-remote\iddseat-remote.inf `
    -Pattern 'UmdfLibraryVersion|UmdfExtensions|HydraSeat_RemoteIDD_v1'
```

**GATE:** `iddseat.dll`, `iddseat-remote.inf` and a `.cat`; signature `Valid`,
`CN=HydraTest`, not expired; `UmdfLibraryVersion` is a number, **not** the
literal `$UMDFVERSION$`.

`dist\` is gitignored. If the folder is gone, run A still stands on its own;
rebuilding the driver is a separate morning, not something to do mid-runbook.

### 1e. Build 26300 still has the class extension the INF names

```powershell
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Wdf\UMDF\IddCx\Versions' -Recurse |
    Get-ItemProperty |
    Where-Object Service |
    Select-Object PSChildName, Service
```

**GATE:** `IddCx0102` is listed. If the build registers something else, the
August package cannot load here; run A only.

### 1f. No device-install policy in the way

```powershell
$r = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions'
if (Test-Path $r) {
    Get-ItemProperty $r | Select-Object DenyUnspecified, DenyDeviceIDs, AllowDeviceIDs
    if (Test-Path "$r\AllowDeviceIDs") { Get-ItemProperty "$r\AllowDeviceIDs" }
} else {
    'no device-install restrictions'
}
```

**GATE:** no restrictions, or `HydraSeat_RemoteIDD_v1` is in the allow-list.
`DenyUnspecified = 1` without it blocks the IDD outright (08-11).

### 1g. What is in the driver store

```powershell
Get-HydraIddPackage
```

Write the answer down. Expected: nothing, or the console `iddseat.inf`.

- **`iddseat-remote.inf` already staged:** skip run A's driverless reading (it
  cannot be driverless) and skip the staging in §4. Go from §2 to §4c.
- **More than one `iddseat-remote` or any `iddsample`:** stop. That is the
  08-17 state where every reading went bad.

---

## 2. Stop the seat

```powershell
.\hydra7-avc.ps1 -Stop
Start-Sleep -Seconds 10
Stop-Service Hydra -Force -ErrorAction SilentlyContinue
Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay_seat -ErrorAction SilentlyContinue
query session
```

**GATE:** no processes listed, and `query session` shows only `services`,
`console` and the `rdp-tcp` listener. **No `teacher` row.**

If `teacher` lingers, `logoff <id>` and look again. A provider connection that
finds an existing `teacher` session gets arbitrated onto it and the run means
nothing. `Start-ProtoRun` refuses to start in that state.

---

## 3. Run A — provider under the patched termsrv, no IDD

```powershell
.\safety-gate.ps1 -Label 'mode4-guard-A-provider'
```

**GATE:** `ALL THREE LEVELS PRESENT`. It throws otherwise; believe it.

```powershell
Remove-Item C:\TestProtocol\createconnection.txt -Force -ErrorAction SilentlyContinue
.\rdsprov-register.ps1 -Register -Apply
```

Read the emergency card it prints the path of. Then:

```powershell
query session
```

**GATE:** a `hydraproto` row in `Listen`, **and** `rdp-tcp` still in `Listen`.

This is already new information. Under RDP Wrapper the listener never appeared.
If it does not appear here, the patched `termsrv.dll` is not loading the
provider; go to §7.

```powershell
Set-ProtoCreds
```

**GATE:** `teacher`, empty `Domain`, `PwLen` matching the real password. The
password sits in the listener key in plain text until §7 deletes the key.

```powershell
Start-ProtoRun -Label A
```

It restarts TermService, writes the trigger, samples every 2 s for 90 s, then
prints `provider.log` and the events from this run only. Everything goes to
`logs\mode4-guard-A-<stamp>.txt`.

### Reading run A

| observation | meaning |
|---|---|
| `explorer=True`, `Active`, gone at ~30 s, console stays `Active` | **08-11 reproduced.** The patch changes what a provider session does. Go on to B with some confidence. |
| `LogonUI=True`, `explorer=False` throughout, reason=12 at ~33 s | same as stock. The patch makes no difference without an IDD. B is still the real test. |
| `PreDisconnect reason=17` early | an IDD is staged after all. Recheck 1g. |
| no `AcceptConnection` in `provider.log` | listener did not fire: credentials, or a stale trigger file. Not a result. |
| console drops to the lock screen | the patch is not in effect for this path. §7. |

### Clean up run A before touching the driver

```powershell
Stop-ProtoRun
.\rdsprov-register.ps1 -Unregister -Apply
```

`-Unregister` ends in `Restart-Service -Force`, which can throw on
`UmRdpService` after the key is already gone. Either way, finish with:

```powershell
Start-Service TermService -ErrorAction SilentlyContinue
(Get-Service TermService).Status
Test-Path C:\TestProtocol\createconnection.txt
.\rdsprov-register.ps1 -Status
```

**GATE:** `Running`, `False`, and no `HydraProto` listener. Only then go on.
---

## 4. Run B — the same, with the remote IDD

### 4a. Stage the driver. This is the boot-affecting change.

```powershell
.\safety-gate.ps1 -Label 'mode4-guard-B-iddremote'
```

**GATE:** all three levels, with the stick. Never `-SkipUsb` for this one.
Note the snapshot folder it prints.

```powershell
pnputil /add-driver C:\Programs\hydra\dist\driver-remote\iddseat-remote.inf /install
Get-HydraIddPackage
```

**GATE:** exactly one `iddseat-remote.inf`. Write down its `oemNN.inf`.

### 4b. Reboot, and prove the machine is intact

```powershell
Restart-Computer
```

After logon, elevated Hydra Shell:

```powershell
Set-Location C:\Programs\hydra
. .\mode4-guard-helpers.ps1
Get-HydraIddPackage
.\tools\termsrv-guard\hydra-termsrv-guard.ps1 -DryRun
.\rdsprov-register.ps1 -Status
query session
```

**GATE:** the package is still there, the guard says `already patched`, there is
no `HydraProto` listener and no `teacher` session.

Then prove the classroom still works with the package staged. This is the check
that tells you whether you could teach tomorrow if you had to stop here:

```powershell
.\hydra7-avc.ps1
```

Seat B up, picture and input. Then:

```powershell
.\hydra7-avc.ps1 -Stop
Start-Sleep -Seconds 10
Stop-Service Hydra -Force -ErrorAction SilentlyContinue
query session
```

**GATE:** as §2. If mode 7 does **not** come up with the package staged, that is
the finding for today: §7, and tell me.

### 4c. Register, and run

```powershell
Remove-Item C:\TestProtocol\createconnection.txt -Force -ErrorAction SilentlyContinue
.\rdsprov-register.ps1 -Register -Apply
query session
Set-ProtoCreds
Start-ProtoRun -Label B
```

Same gates as run A: `hydraproto` in `Listen`, credentials read back correctly.

### Reading run B

The lines to find in `provider.log` are the `GetHardwareId -> hr=..., len=...`
result and `OnDriverLoad ... session=N`. `NotifyCommandProcessCreated` never
fires on this build — its absence means nothing. Also read the driver log at
`C:\HydraLog\iddseat.log`: every IDDCX step through `MonitorArrival` should be
`-> 0x00000000`, and that log is not under `ProgramData`.

| observation | meaning |
|---|---|
| `OnDriverLoad` present, session `Active` at t+90s | contract complete; failure is downstream of the IDD |
| `OnDriverLoad` **absent**, driver log shows `IddCxMonitorArrival -> 0x0`, `PreDisconnect reason=18` | termsrv refuses after the display arrives — the input path is the suspect. See `MODE4-RESULTS.md`, run B. |
| driver log `IddCxMonitorArrival -> 0x...` non-zero | driver-side failure on this build; look at the last driver log entry |
| driver log stops before `MonitorArrival` | adapter init failed; check `AdapterInitStatus` |
| `GetHardwareId -> hr=0x...` non-zero | termsrv's buffer smaller than the hardware ID |
| `PreDisconnect reason=17` | driver never bound; DriverFrameworks log for 2007 or 0xD000000D |
| `reason=12` at ~33 s, no devnode | hardware ID mismatch between provider and INF |
| a different reason code | new. Bring the log. |

The sampler reads every 2 s, so a 0.4 s session may never show in the samples.
The summary says so when that happens; `provider.log` and the
`LocalSessionManager` events are the record.

If the session is alive at the end, the helper leaves it up. Look at it:

```powershell
$s = Get-ProtoSession
$s
Get-CimInstance Win32_Process -Filter "SessionId=$($s.Id)" |
    Select-Object Name |
    Sort-Object Name
Get-PnpDevice -Class Display -PresentOnly |
    Select-Object FriendlyName, Status, InstanceId
```

Then:

```powershell
Stop-ProtoRun
```

---

## 5. Run C — only if run B held

B holding proves nothing about cause until 18 comes back with the patch removed
on this same build. Same driver, same provider, stock DLL.

```powershell
Stop-ProtoRun
.\tools\termsrv-guard\hydra-termsrv-guard.ps1 -Restore
```

**GATE:** `Restored original from ...`. If it refuses, skip run C; do not patch
the DLL by hand to get it.

```powershell
Start-ProtoRun -Label C-stock
```

| observation | meaning |
|---|---|
| reason=18 at ~0.4 s | **Causation shown.** Unpatched: dies. Patched: lives. |
| session holds here too | the patch was not the variable. Build 26300 is, or something since August. Still good news; different explanation. |

**Put the patch back before anything else:**

```powershell
Stop-ProtoRun
.\tools\termsrv-guard\hydra-termsrv-guard.ps1
.\tools\termsrv-guard\hydra-termsrv-guard.ps1 -DryRun
```

**GATE:** `Patched OK`, then `OK: already patched`. Mode 7 does not work
without this.

---

## 6. If you have to stop part-way

Go to §7 from wherever you are. Every step in it is safe to run when the thing
it removes is already gone.

---

## 7. Cleanup — every time, without exception

```powershell
Set-Location C:\Programs\hydra
. .\mode4-guard-helpers.ps1
Stop-ProtoRun
.\rdsprov-register.ps1 -Unregister -Apply
```

```powershell
Start-Service TermService -ErrorAction SilentlyContinue
(Get-Service TermService).Status
Test-Path C:\TestProtocol\createconnection.txt
.\rdsprov-register.ps1 -Status
.\tools\termsrv-guard\hydra-termsrv-guard.ps1 -DryRun
```

**GATE:** `Running`; `False`; no `HydraProto` listener; `already patched`.

Removing the listener key removes the stored password with it.

Now the driver. Use the name from `Get-HydraIddPackage`, typed out, not a
pattern:

```powershell
Get-HydraIddPackage
```

```powershell
pnputil /delete-driver oemNN.inf /uninstall
Get-HydraIddPackage
```

**GATE:** no `iddseat-remote.inf` left. A console `iddseat.inf` that was there
at 1g stays.

```powershell
dism /online /get-drivers /format:table > "C:\Programs\hydra\safety\<the B snapshot folder>\drivers-after.txt"
Restart-Computer
```

Leave the package staged only if you are running the next step within the week
and §4b showed mode 7 healthy with it in place. Otherwise it goes.

---

## 8. Prove the classroom is back

After the reboot:

```powershell
Set-Location C:\Programs\hydra
.\tools\termsrv-guard\hydra-termsrv-guard.ps1 -DryRun
.\rdsprov-register.ps1 -Status
query session
.\hydra7-avc.ps1
```

**GATE:** patched; two listeners, `Console` and `RDP-Tcp`; seat B up on the
panel with picture, sound, keyboard and mouse. Type in it. Do not call the
morning finished until this passes.

Phantom `HYDRASEAT` devnodes under `Enum\SWD\REMOTEDISPLAYENUM` may be left
behind. They are housekeeping (`MODE4-PROVIDER.md`), not a reason to delay §8.

---

## What to bring back

The files in `C:\Programs\hydra\logs\mode4-guard-*.txt`, whole. They carry the
build, the patch state, the staged packages, the samples, `provider.log` and the
events for each run, so nothing has to be remembered.

Also `C:\HydraLog\iddseat.log` — the driver log. Same reasoning; and it is not
under `ProgramData`, so a search for `provider.log` will miss it.

## What each ending means for the plan

- **B holds, C dies:** mode 4's session half is done with no kernel code. The
  input driver stops being a prerequisite and becomes an improvement. Next is
  the texture-sharing test, which decides whether the picture can reach the
  panel cheaply.
- **B dies at 18:** the cap is exonerated with evidence. Next is the same
  texture-sharing test, because it can still end the project before a driver is
  written.
- **B dies at 17, or A differs from August with no IDD involved:** build 26300
  moved something. That is a rebuild-and-baseline question, on a machine that
  is not this one.
