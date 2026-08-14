# MODE 4 RETEST — post-reboot, 2026-08-14

## What this is testing

The 15:04–15:11 runs produced a provider session that reached
`NotifyCommandProcessCreated` (so winlogon DID create the shell process) but sat
at `LogonUI` with no `explorer` for its entire ~33s life, then
`PreDisconnect reason=12`.

Two explanations, and Nathan's counterexample rules out the first:

1. ~~No display device → logon cannot complete~~ — **wrong.** A previous mode 4
   test had `teacher` reach Active.
2. **Logon was too slow and the provider timed out.** `teacher`'s profile was
   rebuilt by the reset, so the first logon loads it cold. `reason=12` at 33s
   looks like a timeout, not a rejection.

This retest warms the profile first. If `explorer` then appears in the provider
session, the driverless path is still open and the IDD may not be needed.

---

## 0. Confirm the machine came back clean

```powershell
cd C:\Programs\hydra; .\test-modes.ps1 -PreflightOnly
```

Also confirm both driver packages are still gone — they were removed before the
last run and the test needs them absent:

```powershell
((pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n" | Where-Object { $_ -match 'iddseat' }).Count
```

**GATE: must be 0.** A staged IDD kills the connection at ~56ms with
`reason=17` and you learn nothing.

---

## 1. Warm teacher's profile — the actual variable

Modes 1–3 still work here, so use them. RDP-Wrapper is loaded, so this is the
normal path.

```powershell
mstsc /v:127.0.0.2 /w:1280 /h:720
```

Log in as `teacher`. **Wait for a real desktop** — taskbar, icons, not just
wallpaper. First logon after a reset can take a while; that is the point.

Then log off from inside the session (Start → user → Sign out), NOT by closing
the window. A clean logoff leaves the profile cached; a disconnect leaves the
session half-alive and you are back where you started.

```powershell
query session
```

**GATE: no `teacher` row at all.** If one lingers, `logoff <id>`.

---

## 2. Provider only loads under stock termsrv

RDP-Wrapper as `ServiceDll` means `TestProtocol_Ext.dll` never loads and the
`hydraproto` listener never appears. Established 08-13, clean A/B.

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\WINDOWS\System32\termsrv.dll' -Type ExpandString
Restart-Service TermService -Force; Start-Sleep 10
```

```powershell
cd C:\Programs\hydra; .\rdsprov-register.ps1 -Register -Apply
```

`-Register` recreates the listener key **EMPTY** every time. This has been
forgotten five times:

```powershell
$k='HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto'; $s=Read-Host 'password for teacher' -AsSecureString; $b=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($s); Set-ItemProperty $k -Name Username -Value 'teacher'; Set-ItemProperty $k -Name Domain -Value ''; Set-ItemProperty $k -Name Password -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)); [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b); Get-ItemProperty $k | Select-Object Username, Domain, @{n='PwLen';e={$_.Password.Length}}
```

**GATE:** `teacher`, empty Domain, PwLen matching the real password.

---

## 3. Trigger and watch it live

The previous runs only looked after the fact. This samples at 8s, 16s and 24s so
you can see whether `explorer` ever appears and when.

```powershell
cd C:\Programs\hydra; Remove-Item C:\ProgramData\Hydra\provider.log, C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; Restart-Service TermService -Force; Start-Sleep 10; New-Item -ItemType File -Force C:\TestProtocol\createconnection.txt | Out-Null; foreach ($t in 8,8,8) { Start-Sleep $t; $id = (query session | Select-String 'hydraproto#').ToString().Trim() -split '\s+' | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1; if ($id) { $n = (Get-CimInstance Win32_Process -Filter "SessionId=$id").Name; "[$([datetime]::Now.ToString('HH:mm:ss'))] session $id : $($n.Count) procs, explorer=$($n -contains 'explorer.exe'), LogonUI=$($n -contains 'LogonUI.exe')" } else { "[$([datetime]::Now.ToString('HH:mm:ss'))] no session" } }
```

```powershell
Get-Content C:\ProgramData\Hydra\provider.log
```

### Reading it

| observation | meaning |
|---|---|
| `explorer=True` at any sample | **profile load was the problem.** Driverless path still open — go to step 4. |
| `LogonUI=True`, `explorer=False` all three, then `reason=12` | logon still not completing even warm. Timeout is not the cause; something else blocks it. |
| `reason=17` early | an IDD is staged after all. Recheck step 0. |
| no session at all | listener did not fire. Credentials, or the trigger file was consumed by an earlier run. |

---

## 4. ONLY IF explorer appeared — the actual measurement

The question test 3 exists to answer: **does a provider session have a
duplicatable display?**

Point `seats.toml` at it. `session` must match the user in that session:

```powershell
Select-String -Path C:\Programs\hydra\seats.toml -Pattern '^session|^display_mode'
```

`display_mode` wants `capture`. Then:

```powershell
.\setup.ps1; Start-Service Hydra; Start-Sleep 8; .\dist\hydractl.exe status
```

```powershell
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

| result | meaning |
|---|---|
| **`seq` advancing** | **DRIVERLESS MODE 4 WORKS.** No iddseat.dll, no 0xD000000D, no signing, no pnputil, nothing that can prevent a boot. Report this loudly. |
| **`STALLED` climbing** | session has no duplicatable display. The IDD is required and 0xD000000D is back on the critical path. |
| **`capture:B: waiting`** | the `session` string in seats.toml does not match. Not a result — fix and rerun. |

`seq` is a seqlock and increments **twice per publish**, so divide by two for a
real frame rate.

---

## 5. Cleanup — do not skip

Leaving the listener registered with the trigger file present makes the next
boot fire a connection on its own.

```powershell
cd C:\Programs\hydra; Stop-Service Hydra -EA SilentlyContinue; Remove-Item C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; .\rdsprov-register.ps1 -Unregister -Apply
```

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\Program Files\RDP Wrapper\rdpwrap.dll' -Type ExpandString
Restart-Service TermService -Force
```

**GATE — modes 1–3 lose their second session without this:**

```powershell
$p=(Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId; (Get-Process -Id $p -Module).ModuleName | Where-Object { $_ -match 'rdpwrap|termsrv' }
```

Both names must be listed.

Emergency undo, physical console only:

```
reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
sc stop TermService
sc start TermService
```

---

## If step 3 shows LogonUI stuck even with a warm profile

Then the timeout theory is dead too, and the next thing to read is the sample's
own connection lifetime — whether 33s is fixed and whether it can be extended:

```powershell
Select-String -Path C:\Programs\rdsprov\Sample\TestProtocol_Ext\*.cpp -Pattern 'timeout|Timeout|Sleep|WaitFor' -Context 2,4
```

`PreDisconnect reason=12` vs `reason=17` is worth pinning down as well —
`reason=17` correlates with a staged driver failing, `reason=12` with these
stalled-logon runs. If those codes are in the sample headers they will name the
condition rather than leaving it inferred.

---

## Restarting mode 4 driver work later

Both packages are unstaged. The build is in `dist\driver-remote` (IddCx 1.10,
UMDF 2.33, WDK 26100, signed CN=HydraTest). To resume:

```powershell
.\safety-gate.ps1 -Label "iddseat-resume"
pnputil /add-driver dist\driver-remote\iddseat-remote.inf /install
```

Next instrument, never used: the WUDF **framework verifier** (`WdfVerifier.exe`),
which logs WHY a driver was rejected rather than only that it was. Or build
Microsoft's own IddCx sample — if that also fails to load, the problem is
environmental rather than in `iddseat.cpp`.
