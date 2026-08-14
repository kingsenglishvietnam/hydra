Read `C:\Programs\hydra\SESSION-2026-08-14.md` first. Then `MODES.md`, and
`git log --oneline -20`. The commit messages run ahead of every .md in the repo —
three documents were wrong about mode 4 for a full day because nobody read the
log.

Do not infer anything that is written on disk. Grep first.

---

# Three tests, in this order

## TEST 1 — monitor separation (2 minutes, do this first)

`NEXT-STEPS.md` PROBLEM 4. The seat monitor sits adjacent to the console
monitor's right edge, so the console pointer walks onto it. That crossing is
what makes the client thumbnail forward input into seat B, which is the
"glitchy cursor" in modes 2 and 3.

It was ~8100px apart before the reset (`0,0` and `11340,0`); it is now 2314px
and side by side.

Settings → System → Display. Drag the seat monitor **above** the console
monitor, offset sideways so only a couple of hundred pixels of edge touch.
Vertical overshoot is rarer than horizontal, and the top edge is already
guarded by title bars.

Verify:

```powershell
Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Screen]::AllScreens | Select-Object DeviceName, Primary, Bounds
```

Then run mode 2 and check the cursor no longer stutters:

```powershell
cd C:\Programs\hydra; .\hydra-start.ps1
```

```powershell
.\dist\mirror.exe B \\.\DISPLAY2
```

---

## TEST 2 — the video glitch A/B (one look, no theory)

Two theories were burned on this already: GPU driver version and codec. Neither
survived. Do not add a third before measuring.

`hydra-view.ps1` starts **two mirrors from the same ring** — fullscreen on
DISPLAY2 and a 1600x900 window. Same frames, two present paths.

```powershell
cd C:\Programs\hydra; $env:HYDRA_GFX='RFX'; .\hydra-view.ps1 -Desktop 2
```

Drag a window around in the seat and look at both mirrors at once.

- **windowed smooth, panel glitchy** → it is `mirror`'s present to DISPLAY2
- **both identical** → the bad frames are already in the ring; the producer is
  the suspect

Confirm the ring is actually healthy while you look:

```powershell
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

`seq` should climb by roughly 90–180 over four seconds. Much less means the
producer is not keeping up and no render fix will help.

**`HYDRA_GFX=RFX`, never `1`** — `1` lets the server pick H.264 and this
`WITH_VAAPI_H264_ENCODING=ON` build crashes. Unset gives plain bitmap updates,
which looks like a glitch bug and is not.

Full stop between tests, always:

```powershell
Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay -EA SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; query session
```

then `logoff <teacher id>`.

---

## TEST 3 — driverless mode 4 (the interesting one)

See `SESSION-2026-08-14.md` §7a. The question is **not** why the IddCx driver
will not load. It is whether it is needed at all.

The RDS provider already creates a real logged-in session with no RDP in the
path (`ec995bb`, tag `rdsprov-session`) — `explorer`, `dwm`, `winlogon`, a full
desktop. DWM composites to something. Modes 1–3 already extract pixels from a
session with **no driver** via DDA → shared ring → `mirror`.

So: **does `EnumOutputs` return an output inside a provider-created session?**

Yes → mode 4 needs no driver at all. No `iddseat.dll`, no `0xD000000D`, no
signing, no `pnputil`, nothing in the path that can prevent a boot.

No → the IDD really is required and `0xD000000D` is back on the critical path.

### Preconditions

The provider only loads under **stock termsrv** — `rdpwrap.dll` as `ServiceDll`
means `TestProtocol_Ext.dll` never loads and the `hydraproto` listener never
appears. Clean A/B, established 08-13.

Modes 1–3 lose their second session while this is set. **Restore it afterwards.**

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\WINDOWS\System32\termsrv.dll' -Type ExpandString
Restart-Service TermService -Force; Start-Sleep 10
```

```powershell
cd C:\Programs\hydra; .\rdsprov-register.ps1 -Register -Apply
```

Credentials — `-Register` recreates the listener key EMPTY every time, and this
has bitten five times:

```powershell
$k='HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto'; $s=Read-Host 'password for teacher' -AsSecureString; $b=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($s); Set-ItemProperty $k -Name Username -Value 'teacher'; Set-ItemProperty $k -Name Domain -Value ''; Set-ItemProperty $k -Name Password -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)); [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b); Get-ItemProperty $k | Select-Object Username, @{n='PasswordSet';e={[bool]$_.Password}}
```

**GATE:** `teacher` / `True`.

### Trigger

```powershell
cd C:\Programs\hydra; Remove-Item C:\ProgramData\Hydra\provider.log, C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; Stop-Service TermService -Force; Start-Sleep 3; Start-Service TermService; Start-Sleep 10; New-Item -ItemType File -Force C:\TestProtocol\createconnection.txt | Out-Null; Start-Sleep 30; query session; Get-Content C:\ProgramData\Hydra\provider.log
```

Note the session id from `query session`, then confirm it has a real desktop:

```powershell
Get-CimInstance Win32_Process -Filter "SessionId=<id>" | ForEach-Object { [PSCustomObject]@{ Name=$_.Name; User=(Invoke-CimMethod -InputObject $_ -MethodName GetOwner).User } } | Sort-Object Name
```

Want `explorer.exe`, `dwm.exe`, `winlogon.exe`.

### The measurement

Point `seats.toml` at that session, `display_mode = "capture"`, deploy, and read
the ring:

```powershell
.\setup.ps1; Start-Service Hydra; Start-Sleep 8; .\dist\hydractl.exe status
```

```powershell
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

- **`seq` advancing** → driverless mode 4. Report this loudly; it changes the
  project.
- **`STALLED` climbing** → no duplicatable display in a provider session. The
  IDD is required after all.
- **`capture:B: waiting`** → the `session` string in `seats.toml` does not match.
  Not a result.

### Cleanup — do not skip

Leaving the listener registered with the trigger file present makes the next
boot fire a connection on its own.

```powershell
cd C:\Programs\hydra; Remove-Item C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; .\rdsprov-register.ps1 -Unregister -Apply
```

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\Program Files\RDP Wrapper\rdpwrap.dll' -Type ExpandString
Restart-Service TermService -Force
```

Emergency undo if the provider takes RDP down — run at the physical console:

```
reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
sc stop TermService
sc start TermService
```

---

# Standing rules

- **One boot-affecting change per reboot.** The 08-12 boot carried four at once
  and cost an OS install with no way to tell which caused it.
- **`safety-gate.ps1` before any driver install, class filter edit, CI/WDAC, BCD
  or RDP-Wrapper work.** It refuses if any of the three undo levels is missing.
- **Never run two modes at once** — two clients on one session, or two producers
  on one ring, wedges the RDP stack and costs a reboot.
- **Never `HYDRA_GFX=1`.**
- **Do not install or update Logitech software.** Installing a Logitech mouse
  driver *inside the seat session* is the confirmed PROBLEM 1 trigger — it tore
  down that session's display stack, 1806 capture retries, only a client
  reconnect cleared it.
- Logs under `C:\ProgramData\Hydra\logs` are **startup-only**. Check
  `LastWriteTime` before believing any of them. `hydra-shm.ps1` reads live state.
- `hydractl status` needs elevation or returns `err 5`.
