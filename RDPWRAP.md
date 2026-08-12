# RDP-Wrapper — install and maintain

Last rebuilt: 2026-08-13, after the reset documented in `INCIDENT-2026-08-12.md`.

RDP-Wrapper is what gives seat B a second concurrent session. Stock Windows 11
client editions allow one interactive session; the wrapper shims `termsrv.dll`
rather than patching it, so Windows updates don't permanently break it — they
break the *config*, which is a file you replace.

**Do this rested.** It is the only remaining install that modifies how a system
binary behaves.

---

## The one thing that actually matters

<cite index="18-1">The default `rdpwrap.ini` from the 2017 release does not include 24H2 offsets. Use the community-maintained INI from the `sebaxakerhtc/rdpwrap.ini` repo — it is updated within days of each Patch Tuesday.</cite>

Everything else here is routine. This is the step that decides whether it works.

The wrapper depends on byte offsets inside your exact `termsrv.dll` build. <cite index="16-1">New Windows builds sometimes ship before the ini catches up, which creates a short window where multi-session breaks until a contributor adds the new offsets. If you rely on this, don't install Windows feature updates immediately — wait until the ini supports the new build.</cite>

**For a classroom machine, that's a scheduling rule, not a footnote.** A Patch
Tuesday reboot can silently cost you seat B on a teaching morning.

---

## 0. Record the target first

```powershell
(Get-Item C:\Windows\System32\termsrv.dll).VersionInfo.FileVersion
```

Write it down. This is the build the ini has to support, and it's the first thing
to check whenever seat B stops working.

Commit it alongside the driver inventory so it survives the machine.

---

## 1. Defender will flag it

<cite index="18-1">It's flagged because the technique resembles what some malware does. The source is public and the hash is verifiable on GitHub. Add a permanent exclusion for `C:\Program Files\RDP Wrapper\`.</cite>

Set the exclusion **before** installing, or Defender will quarantine files
mid-install and you'll debug a corrupted install instead of a clean one.

```powershell
Add-MpPreference -ExclusionPath 'C:\Program Files\RDP Wrapper'
```

Elevated. Verify:

```powershell
(Get-MpPreference).ExclusionPath
```

## 2. Install the wrapper

Get RDPWrap 1.6.2 from `github.com/stascorp/rdpwrap/releases`. Extract somewhere
outside `Program Files`, then from an **elevated** prompt in that folder:

```
RDPWInst.exe -i -o
```

`-o` installs to `C:\Program Files\RDP Wrapper` rather than the default location,
which keeps it aligned with the Defender exclusion above.

## 3. Replace the ini — the step people skip

The bundled ini is from 2017 and will not know your build. Get the current one
from `github.com/sebaxakerhtc/rdpwrap.ini`, then:

```powershell
Stop-Service TermService -Force
```

```powershell
Copy-Item .\rdpwrap.ini 'C:\Program Files\RDP Wrapper\rdpwrap.ini' -Force
```

```powershell
Start-Service TermService
```

`TermService` holds the ini open while running, so the stop is not optional.

## 4. Verify

Run `RDPConf.exe` from the install folder. <cite index="16-1">You want all-green: Wrapper installed yes, Wrapper running yes, Listener state listening, and `[fully supported]` next to your termsrv version.</cite>

<cite index="16-1">`[not supported]` means the ini has no offsets for your exact build — get a newer community ini, or wait for it to be updated. "Listener state: Not listening" means the service isn't running or the ini lacks your build: restart TermService, update the ini.</cite>

Then confirm from the shell rather than trusting the GUI:

```powershell
Get-Service TermService | Select-Object Status, StartType
```

```powershell
query session
```

## 5. Real test

Two sessions at once is the only proof that counts.

```powershell
cd C:\Programs\hydra; .\dist\hydrardp.exe B teacher '' 127.0.0.2
```

Then, in a second window:

```powershell
cd C:\Programs\hydra; .\dist\mirror.exe B \\.\DISPLAY2
```

```powershell
query session
```

Two active sessions — `console` as `user`, and one as `teacher` — means it works.

**One client only.** `hydra-start.ps1` launches `sdl-freerdp`, and two clients on
one session wedges the stack.

---

## Failure modes, in the order they actually happen

| Symptom | Cause | Fix |
|---|---|---|
| `[not supported]` in RDPConf | ini lacks your `termsrv.dll` build | newer community ini |
| Listener not listening | `TermService` stopped, or ini mismatch | restart service, then check ini |
| Worked yesterday, broken today | Windows Update replaced `termsrv.dll` | re-record version (§0), new ini |
| Files vanish after install | Defender quarantine | exclusion (§1), reinstall |
| All-green but still one session | Group Policy limiting connections | check `gpedit` RDS connection limits |

**The "worked yesterday" row is the one you'll hit most.** It is not a Hydra fault
and it is not worth debugging from the Hydra side — check `termsrv.dll`'s version
first, every time.

---

## Uninstall

```
RDPWInst.exe -u
```

Clean removal — it never modified `termsrv.dll`, so there is nothing to restore.
This is genuinely reversible, unlike the driver work.

---

## Licensing

<cite index="19-1">RDP Wrapper is open source, but its usage may violate Microsoft's licensing agreements. It is recommended for personal or educational use only, and is not recommended in business environments.</cite>

Worth knowing where the line is for a fee-paying language centre. The licensed
alternative in this space is Thinstuff XP/VS Server if this ever needs to be
above board rather than merely working.

---

## Standing rules

1. **Recovery stick present before installing.** Same precondition as driver work.
   Lower risk here — the wrapper cannot prevent a boot — but the rule is cheap.
2. **Record `termsrv.dll`'s version after every Windows Update**, and check the ini
   before a teaching day rather than during one.
3. **Defer feature updates on this machine.** A build that outruns the community ini
   costs you seat B until a contributor catches up.
4. **`RDPWInst.exe -u` is the undo.** Try it before anything more drastic.
