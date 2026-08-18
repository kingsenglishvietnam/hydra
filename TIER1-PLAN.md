# TIER1-PLAN.md — a seat session with no RDP protocol in it

Written 2026-08-18.

**Goal.** Seat B gets a real Windows session whose pixels are never encoded,
transmitted or decoded. No wire protocol, no client, no capture, no ring, no
mirror. The session simply has a display and `mirror` — or nothing at all —
puts it on the panel.

**Not** the elimination of `TermService`. That is tier 2, it needs kernel-mode
code in the input and display stacks, and it is a multi-year project that the
commercial vendors have found hard. Tier 1 is the realistic ceiling on Windows,
and half of it is already built.

---

## What is already done

**`HydraProto` creates real Windows sessions with no RDP in the path.**
Commit `ec995bb`, tag `rdsprov-session`: *"created a Windows session with no RDP
in the path."* `RESUME-2026-08-12` records that session running `explorer`,
`dwm`, `winlogon` — a full desktop. The provider implements ~15 methods; there
is no socket and no protocol anywhere in it.

**The provider does not need RDP-Wrapper.** Established 2026-08-14: with
`ServiceDll` at stock `termsrv.dll`, `query session` showed the console session
and `hydraproto#0` **concurrently**. `NEXT-STEPS.md` had listed this as an open
question and called its own answer inference. It is fact.

So the session half of tier 1 works. What is missing is the **display** half.

---

## The one thing blocking it

With no IDD staged, a provider session authenticates cleanly — Security 4624,
Logon Type 10 RemoteInteractive, no 4625 — and then **stalls at `LogonUI`
forever**. Winlogon 7001 never fires. Identical at 8s, 16s and 24s, warm profile
and cold.

Winlogon appears to need a display target to complete the desktop switch.

Every attempt to supply one via a custom IddCx driver has failed with
`0xD000000D` — UMDF refusing the driver at load level 0, before `DriverEntry`.
About fifteen causes eliminated with evidence. See `MODE4-STEP2.md`.

---

## The route that has never been tried

**`SWD\REMOTEDISPLAYENUM\RDPIDD_INDIRECTDISPLAY` loads fine on this machine.**

Five `rdpidd.inf` instances sit in the Display class right now. **Microsoft's own
remote-session indirect display driver works here today.** It is what gives an
ordinary `mstsc` session its display — the thing modes 1, 2 and 7 have been
capturing from all along.

The question nobody has asked: **can a `HydraProto` session use `rdpidd` instead
of a driver of ours?**

If yes, tier 1 needs no custom driver at all, `0xD000000D` becomes irrelevant,
and the whole five-day IddCx problem is bypassed rather than solved.

The reason to think it might: the provider's own log line is
`EnableWddmIdd(1)  <-- termsrv telling us the mode`. **termsrv is telling the
provider which display mode it has chosen**, not asking. Something on the
termsrv side decides an IDD is wanted and then goes looking for one. If that
lookup can be pointed at `rdpidd`, or if `rdpidd` can be made to match the
provider's devnode, the display arrives for free.

---

## Step 0 — a clean baseline. Non-negotiable.

Microsoft's own IddCx sample loaded on this machine on the morning of
2026-08-17 and would not by that evening, unchanged. The Display class has had
~15 package installs and removals, hand-deleted instance keys, and dozens of
devnode create/removes. **Any measurement taken now is unreadable.**

Options, cheapest first:

```powershell
$e = (pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n"
for ($i=0; $i -lt $e.Count; $i++) { if ($e[$i] -match 'Original Name:\s+(iddsample|iddseat)') { "{0} <- {1}" -f ($e[$i-1] -replace '.*:\s+',''), ($e[$i] -replace '.*:\s+','') } }
```

Delete every one of those — keep `mttvdd`, that is mode 6. Then reboot and
confirm the sample loads again. If it does, the accumulation was the problem and
you have a baseline.

If it still does not, **reset the machine, keep files.** `REBUILD.md` is written
for exactly this and takes about an hour. Do not start tier 1 work on a machine
whose driver state you cannot trust — that is the single clearest lesson of the
08-17 sessions.

---

## Step 1 — find out what termsrv looks for

Before writing anything. This is a reading exercise and it may end the project
early, in either direction.

```powershell
Get-PnpDevice -Class Display | Select-Object FriendlyName, Status, InstanceId
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\SWD\REMOTEDISPLAYENUM' -Recurse -EA SilentlyContinue | Select-Object PSChildName, HardwareID, Service
```

Then, with an ordinary `mstsc` session **open**, look at what the RD stack
created for it:

```powershell
mstsc /v:127.0.0.2 /w:1280 /h:720
```

```powershell
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\SWD\REMOTEDISPLAYENUM' | Select-Object PSChildName
```

**What to look for:** the devnode the RD stack makes for a live session, its
hardware ID, and which INF claims it. `rdpidd.inf` is in
`C:\Windows\INF\rdpidd.inf` and is readable:

```powershell
Get-Content C:\Windows\INF\rdpidd.inf
```

Compare it against `iddseat-remote.inf`. Microsoft's remote IDD is the reference
implementation of the thing that has never worked here, and its INF is on disk.

**This is the highest-value hour in the whole plan.** It may show that `rdpidd`
claims a hardware ID the RD stack always generates — in which case a provider
session might get it automatically, and the only reason it did not is that
something else was staged and matched first.

---

## Step 2 — trigger a provider session with NO custom IDD staged

The experiment is: does the RD stack give a provider session `rdpidd`, if
nothing of ours is competing?

```powershell
((pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n" | Where-Object { $_ -match 'iddsample|iddseat' }).Count
```

**Must be 0.** Then:

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\WINDOWS\System32\termsrv.dll' -Type ExpandString
Restart-Service TermService -Force; Start-Sleep 10
```

RDP-Wrapper must be out of the way or `TestProtocol_Ext.dll` never loads at all.

```powershell
cd C:\Programs\hydra; .\rdsprov-register.ps1 -Register -Apply
```

**`-Register` recreates the listener key EMPTY every time.** Six occurrences so
far. `Domain` must be **empty**, not the machine name:

```powershell
$k='HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto'; $s=Read-Host 'password for teacher' -AsSecureString; $b=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($s); Set-ItemProperty $k -Name Username -Value 'teacher'; Set-ItemProperty $k -Name Domain -Value ''; Set-ItemProperty $k -Name Password -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)); [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b); Get-ItemProperty $k | Select-Object Username, Domain, @{n='PwLen';e={$_.Password.Length}}
```

Trigger:

```powershell
cd C:\Programs\hydra; Remove-Item C:\ProgramData\Hydra\provider.log, C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; Restart-Service TermService -Force; Start-Sleep 10; New-Item -ItemType File -Force C:\TestProtocol\createconnection.txt | Out-Null; Start-Sleep 30; query session; Get-Content C:\ProgramData\Hydra\provider.log
```

Then, **while the session is alive**, look for a display:

```powershell
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\SWD\REMOTEDISPLAYENUM' | Select-Object PSChildName
Get-PnpDevice -Class Display | Select-Object FriendlyName, Status, InstanceId
```

| observation | meaning |
|---|---|
| a new `REMOTEDISPLAYENUM` devnode with Status OK | **the RD stack gave it `rdpidd`.** Go to step 3. |
| no new devnode, session stalls at `LogonUI` | the stack wants a driver we must supply. Back to `MODE4-STEP2.md`. |
| a devnode in `CM_PROB_FAILED_ADD` | something is still staged and matching first. Recheck step 2's gate. |

---

## Step 3 — if the session gets a display

Then tier 1 is essentially done and the rest is plumbing you already have.

```powershell
$id = (query session | Select-String 'hydraproto#').ToString().Trim() -split '\s+' | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1
Get-CimInstance Win32_Process -Filter "SessionId=$id" | Select-Object Name | Sort-Object Name
```

**`explorer.exe` and `dwm.exe` present, `LogonUI.exe` absent** means logon
completed — which has never happened without a display.

Then point the existing capture at it:

```powershell
Select-String -Path C:\Programs\hydra\seats.toml -Pattern '^session|^display_mode'
```

`display_mode = "capture"`, `session` matching that user, then:

```powershell
.\setup.ps1; Start-Service Hydra; Start-Sleep 8; .\dist\hydractl.exe status
.\hydra-shm.ps1; Start-Sleep 4; .\hydra-shm.ps1
```

`seq` and `frame` advancing means **tier 1 is real**: a Windows session with no
RDP protocol in it, feeding the panel through machinery that already works.

`mirror B \\.\DISPLAY2` then puts it on the student's monitor exactly as modes
1, 2 and 6 do.

---

## Step 4 — cleanup, every time

Leaving the listener registered with the trigger file present makes the next
boot fire a connection on its own.

```powershell
cd C:\Programs\hydra; Stop-Service Hydra -EA SilentlyContinue; Remove-Item C:\TestProtocol\createconnection.txt -Force -EA SilentlyContinue; .\rdsprov-register.ps1 -Unregister -Apply
```

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' -Name ServiceDll -Value 'C:\Program Files\RDP Wrapper\rdpwrap.dll' -Type ExpandString
Restart-Service TermService -Force; Start-Sleep 5
$p=(Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId; (Get-Process -Id $p -Module).ModuleName | Where-Object { $_ -match 'rdpwrap|termsrv' }
```

**Both names must be listed** or modes 1–3, 6 and 7 silently lose their second
session.

Emergency undo, physical console only:

```
reg delete "HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto" /f
sc stop TermService
sc start TermService
```

---

## What tier 1 buys, honestly

Removes the encode/decode round trip and the client process. Shorter pixel path,
lower CPU, less latency.

**It does not fix the real limitation.** Seat B still gets **software rendering**
— no GPU acceleration in the session, because that is a property of a remote
session, not of the protocol carrying it. Video playback is limited either way.

Only tier 2 (kernel driver, partitioning one session's input and display stacks)
or GPU passthrough per VM, or Linux, fixes that.

So tier 1 is elegance and efficiency, not capability. **Mode 7 already works and
teaches fine.** Do this because the problem is interesting and the pieces are
half built, not because a lesson depends on it.

---

## If step 2 says no

The RD stack wants a driver we must supply, and that is `MODE4-STEP2.md` again —
build from Microsoft's IddCx sample with `IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER`
and `USE_SMALLEST_MODE`, on a clean machine.

The known risk there: `IddCxAdapterDisplayConfigUpdate2` needs IddCx 1.10, and
the only registered class extension on this machine is `IddCx0102`
(`Control\Wdf\UMDF\IddCx\Versions\1\2` → `Service = IddCx0102`). If a driver
declaring `IddCx0102` cannot make that call, remote-session IDDs may not be
reachable on this build of 24H2 at all.

That is a real possible outcome. Establish it in the first hour rather than the
third day.

---

## Traps, all paid for already

- **Reboot when Windows says a reboot is required.** `CM_PROB_NEED_RESTART`
  makes every downstream result meaningless. Two false conclusions in one
  afternoon.
- **Never stage two packages claiming the same hardware IDs.** It broke a driver
  that had been loading fine an hour earlier.
- **Check event timestamps before concluding anything.** Three false readings.
- **Verify a patch landed before building.** One edit was silently lost to a
  file restore and produced twenty minutes debugging a binary that did not
  contain it.
- **Filter `pnputil` output precisely.** A loose `oem22[0-9]` pattern deleted two
  unrelated system drivers.
- **Stale `SessionId` devnodes in `CM_PROB_FAILED_ADD` block retriggering.**
  Reboot between attempts.
- **A registered provider locks its own DLL.** Unregister → build → register.
- **`safety-gate.ps1` before every `pnputil /add-driver`.**
