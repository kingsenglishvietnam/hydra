# Hydra — The Dummy's Guide, A to Z

What to do next, in order, with every command spelled out. Three phases:
**measure** (tonight, free), **decide** (one table), then **one of two paths** —
ASTER (D–H) or Hydra bring-up (I–Z). You cannot do this wrong by following the
letters in order; every step says what you should see before moving on.

Ground rules for every step:
- "Elevated PowerShell" = right-click PowerShell → *Run as administrator*.
- Any `.\build.ps1` run means: `powershell -ExecutionPolicy Bypass -File .\build.ps1`
  from the repo root (`C:\Programs\hydra-3` or wherever the latest extract lives).
- When something fails, check the **Appendix: quick fixes** table before anything else —
  every error we have already met is in it.

---

## Phase 0 — Measure the problem (tonight, ~15 minutes, costs nothing)

The desync you're fighting is two clocks: video rides the RDP graphics pipeline
(encode → channel → decode, elastically buffered), audio rides `rdpsnd` with its
own buffer. No tuning merges two clocks. But you can **delete the audio clock
right now** and hear exactly what remains.

### A. Route the RDP session's audio to the real sound card

1. Open **mstsc** (don't connect yet) → **Show Options** → **Local Resources** tab.
2. Under **Remote audio** → **Settings…** → Remote audio playback →
   select **Play on remote computer** → OK.
3. Connect to the RDP-Wrapper session as usual.

"Remote computer" is this same physical machine, so audio now exits the actual
sound card directly — zero RDP involvement, zero `rdpsnd` buffer. Only the video
lane still goes through the codec.

### B. Play a sync-test video and judge the gap

In the RDP session, search YouTube for **"audio video sync test"** — any of the
top results with a repeating *flash + beep* pattern works. Watch for ~30 seconds.

Expected result: the desync **inverts** — the beep now *leads* the flash by
roughly the codec pipeline's latency, and it's constant rather than drifting.
That audible gap is precisely the quantity Hydra's blit path deletes (and that
ASTER never has). You are listening to the thing you're deciding about.

Also worth 5 minutes: play a normal video (the actual lesson material) and ask
the only question that matters — *would a student notice?*

### C. Decide

| What you heard in B | Do this |
|---|---|
| Gap is small/constant enough to shrug at | **Stop here.** RDP-Wrapper + play-on-remote-computer is your free working setup. Hydra becomes an optional weekend project, not a blocker. |
| Gap is a dealbreaker, and this needs to *work* soon (students, term time) | **Path A: ASTER** → steps D–H. Certain, shipping, per-seat audio built in. |
| Gap is a dealbreaker, and you have appetite for bring-up + one hard unknown | **Path B: Hydra** → steps I–Z. Free, yours, architecturally correct — with Risk #1 still unproven. |

The paths aren't exclusive: ASTER has a trial, so D–H can run while Hydra waits.
Doing D–H first and I–Z later is a perfectly sane order.

---

## Path A — ASTER (steps D–H)

ASTER (by IBIK) does single-session true multiseat via its own kernel hooks:
each seat gets a real GPU output and a *natively bound* audio device. No
transport, no codec, no sync problem by construction. It is the commercial
version of the thing Hydra's architecture doc scoped out as "undocumented
kernel patching."

### D. Clean the field first

Two input-hooking drivers fighting each other produces garbage data. Before the
trial, from an elevated prompt:

```
:: if the Interception driver is installed (it is, from the v3 work):
install-interception.exe /uninstall
```

If Hydra's service/driver ever got installed (steps U–V), remove them too — see
**Appendix: full rollback**. Then **reboot**.

RDP-Wrapper can stay installed; ASTER simply doesn't use it.

### E. Install the trial

Download ASTER from **ibik.ru** (English site available), install, reboot when
asked. Trial length and pricing are on the site — check there rather than
trusting anything secondhand.

### F. Configure the second place

Open the **ASTER Control** panel:
1. **Monitors tab** — drag your second physical display onto *Place 2*.
2. **Keyboards/mice** — drag the second keyboard + mouse onto Place 2.
   (ASTER identifies devices live as you wiggle/type — same idea as `--learn`.)
3. **Audio** — drag the second audio endpoint (USB headset) onto Place 2.
   This is the per-seat audio binding nothing else in this document has.

### G. Enable and log in

Enable ASTER in the control panel → reboot → both places show a logon screen.
Log Place 2 in as the second user account.

### H. The verdict test

Run step B's sync-test video **on Place 2**, audio through Place 2's headset.
There is no transport in the path, so it should be simply, boringly local.
If it is: that's your answer — license it, and Hydra retires to research status
with honor. If ASTER itself misbehaves on your hardware (Surface Book 3 +
external GPU arrangements can be quirky): the trial cost you nothing, and Path B
is still open.

---

## Path B — Hydra bring-up (steps I–Z)

Everything below has one purpose: eliminate every mechanical unknown so that
step X asks the *only* real question (Risk #1) cleanly. Do not skip letters.

### I. Get current and rebuild

1. Re-extract the **latest** `hydra.zip` over your working folder (the current
   one fixes `mirror.exe` monitor listing, `onecore.lib`, and the `childLog`
   shadow — if your `hydrad.cpp` line 233 says `hlog`, you're on stale source).
2. From the repo root:

```
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Expect: 6 built, `seat_router.c` SKIPPED (next step fixes that).

### J. Interception SDK → seat_router builds

1. Download the Interception release zip from **github.com/oblitum/Interception**
   (Releases → `Interception.zip`).
2. From the zip's `library` folder, copy into the repo's `input\` folder:
   - `interception.h`
   - `x64\interception.lib`
3. Copy `x64\interception.dll` into `dist\` (the exe needs it at runtime).
4. Re-run `build.ps1`. Expect: **7 built, 0 failed.**

### K. Interception *driver* present

The SDK above is the userland half; the kernel driver is what actually captures
input. You installed it during the v3 work. If in doubt, from the zip's
"command line installer" folder, elevated:

```
install-interception.exe /install
```

then reboot. (If you did step D for an ASTER trial, you *must* redo this.)
The functional proof is the next step working.

### L. Learn the input device numbers

Sit at the physical console with both keyboards/mice attached. From `dist\`:

```
.\seat_router.exe --learn
```

Type on each keyboard, move each mouse; note which **number** each one reports
(keyboards are 1–10, mice 11–20). Ctrl-C when done. Write the seat-B pair down.

### M. Learn the monitor device names

```
.\mirror.exe
```

No arguments — it lists every attached display's stable `\\.\DISPLAYn` device
name, resolution, position, and which is primary. Note which name is seat B's
physical panel and which is seat A's (for `confine_monitor`).

### N. Edit `seats.toml`

Open `dist\seats.toml`. Fill in, for seat B: `kbd` and `mouse` from step L,
`monitor` from step M (single quotes, e.g. `'\\.\DISPLAY2'`), `port` can stay
`56789`, `session = "auto"`, `edid` matching the panel's native mode. Set
`[hostA] confine_monitor` to seat A's device name.

### O. Install the WDK

The driver needs the Windows Driver Kit — a real install, ~20 min:
1. In **Visual Studio Installer**, confirm the **Windows 11 SDK** component is
   installed (it ships with the C++ workload; note its version).
2. Download the **WDK** from
   `learn.microsoft.com/windows-hardware/drivers/download-the-wdk` — pick the
   WDK matching your SDK version.
3. At the end of WDK setup, **leave the "Install Windows Driver Kit Visual
   Studio extension" box checked.** That checkbox is the whole point.

### P. Create the driver project

1. VS 2022 → New Project → search **UMDF** → pick the **empty User Mode Driver
   (UMDF V2)** template → name it `iddseat`, put it anywhere (e.g. `driver\`).
2. Delete the template's placeholder source files.
3. **Add → Existing Item**: the repo's `iddseat\iddseat.cpp`, `iddseat\iddseat.h`,
   and `iddseat\iddseat.inf`.

### Q. Configure the project (x64 / Release selected, all of these under Project → Properties)

1. **C/C++ → Language → C++ Language Standard** → `/std:c++17`.
2. **C/C++ → General → Additional Include Directories** → add the repo's
   `common\` folder (the driver includes `../common/hydra_edid.h` etc. — adding
   the repo root also works).
3. **Linker → Input → Additional Dependencies** → add `IddCx.lib`.
4. In Solution Explorer, right-click `iddseat.inf` → Properties → **Item Type =
   Inf** (this makes the build run `stampinf` + `inf2cat` automatically).
5. **Driver Signing → General → Sign Mode = Off.** We sign manually in step S —
   one deterministic script instead of VS's cert-store scavenger hunt.

### R. Build it

Build → x64 Release. First driver build of any project surfaces friction —
that's normal, not a design problem (compare: `hydrad` needed three rounds).
Paste any errors back to me verbatim. Success = a package folder containing
**`iddseat.dll` + `iddseat.inf` + `iddseat.cat`**. Copy all three into `dist\`.

### S. Create + trust a test cert, sign the catalog

Elevated PowerShell, from `dist\`:

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=HydraTest" -CertStoreLocation Cert:\CurrentUser\My
& signtool sign /fd sha256 /sha1 $cert.Thumbprint .\iddseat.cat
$pw = ConvertTo-SecureString "hydra" -AsPlainText -Force
Export-PfxCertificate -Cert $cert -FilePath $env:TEMP\hydra.pfx -Password $pw | Out-Null
Import-PfxCertificate -FilePath $env:TEMP\hydra.pfx -Password $pw -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-PfxCertificate -FilePath $env:TEMP\hydra.pfx -Password $pw -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
```

(`signtool` comes from the SDK/WDK; if not on PATH, run from a Native Tools
prompt or use its full path under `C:\Program Files (x86)\Windows Kits\10\bin\...`.)

### T. Test-signing mode on

```
bcdedit /set testsigning on
```

**Reboot.** A "Test Mode" watermark appears bottom-right of the desktop — that's
correct and required. There is no unsigned load path for a Display-class driver;
this is Windows' rule, not Hydra's.

### U. Install the driver package

Elevated, from `dist\` (dll + inf + signed cat together):

```
pnputil /add-driver .\iddseat.inf /install
```

Expect "Driver package added successfully." Nothing visible happens yet —
hydrad creates the actual monitor instances.

### V. Install and start the service

This is what fixes the `1314` from your smoke test: `WTSQueryUserToken` needs
`SeTcbPrivilege`, which only **SYSTEM** holds — an elevated admin prompt still
doesn't. The SCM hands the service a SYSTEM token. Elevated, from `dist\`:

```powershell
.\hydrad.exe install
Start-Service Hydra
.\hydractl.exe status
```

`status` should show the seat-B virtual monitor present and each helper
`running` (or `waiting` for a session that isn't logged in yet). Logs:
`C:\ProgramData\Hydra\logs\`. The service auto-starts on boot from now on;
`Stop-Service Hydra` + `Set-Service Hydra -StartupType Disabled` parks it.

### W. Confirm the virtual monitor exists

Right-click desktop → **Display settings**. You should see one more display than
you have physical panels — 1920x1080, the "Hydra Virtual Seat Display." If yes:
every mechanical layer works. One question remains.

### X. The moment of truth (Risk #1, stated honestly)

Log the RDP-Wrapper session B in. Look at seat B's physical panel (where
`mirror` is presenting). **Three possible outcomes:**

1. **The panel shows session B's desktop.** Risk #1 didn't bite, or bit softly.
   Go to Y. (I will be genuinely, pleasantly surprised.)
2. **The panel shows an empty extension of *your* (console) desktop** — your
   wallpaper, your cursor can wander onto it. This is Risk #1 manifesting
   exactly as the architecture doc predicted: the virtual monitor attached to
   the console session's desktop, not session B's. Windows binds indirect
   displays to the console by default; rebinding one to a specific TS session
   is the undocumented part. Go to Z.
3. **Black panel / mirror exits / Device Manager shows the display device with
   a yellow bang (Code 52).** Mechanical, not architectural — signing (redo
   S–T), INF, or a name mismatch. Check the Appendix table, then
   `C:\ProgramData\Hydra\logs\mirror_B.log`.

Do not sink days into outcome 2 alone. It's the known hard problem; it needs a
diagnosis session, not brute force.

### Y. Victory lap (only from outcome 1)

Re-run step B's sync-test video *in seat B, on seat B's panel*. There is no
codec in the path anymore — the gap should be gone, not smaller. Audio: log each
seat in as its own user account and set that account's default playback device
to its own USB headset (Settings → Sound). Per-account defaults give you
serviceable isolation. Then run a real lesson on it before trusting it with
students.

### Z. Report back (from outcome 2, or anything weird)

Capture and bring to me:
1. `hydractl status` output,
2. `C:\ProgramData\Hydra\logs\hydrad.log` and `mirror_B.log`,
3. one sentence describing what seat B's panel actually shows,
4. elevated: `pnputil /enum-devices /class Display` output.

That's a diagnosis session, and it's the genuinely experimental part — the same
part ASTER's decade of kernel work exists to solve. If outcome 2 proves sticky,
the honorable exit is D–H: Hydra's display idea was still right, the input stack
still works, and the license buys the one piece nobody sanely rebuilds alone.

---

## Appendix: quick fixes (errors we have already met)

| Symptom | Fix |
|---|---|
| `1314` on `WTSQueryUserToken` | You ran `hydrad.exe run` from a prompt. Fine for smoke tests; helpers need the **service** (step V). |
| `build.ps1 ... not digitally signed` | Run via `powershell -ExecutionPolicy Bypass -File .\build.ps1`, or `Set-ExecutionPolicy -Scope Process Bypass -Force` first. |
| `cl : not recognized` | Not in a dev environment. Native Tools prompt, or let `build.ps1` self-arm, or `Enter-VsDevShell` per BUILD.md. |
| `Join-Path ... 'Path' is null` from build.ps1 | Stale script (pre `-products *` vswhere fix). Re-extract latest zip. |
| `C2064 ... not a function taking 4 arguments` in hydrad | Stale source (`hlog` shadow). Re-extract latest zip. |
| `LNK2019: SwDeviceCreate` | Stale link line. It's `onecore.lib`, not `cfgmgr32.lib` — latest zip has it everywhere. |
| `seat_router.c SKIPPED` | Step J not done (Interception SDK files missing from `input\`). |
| seat_router builds but won't start | `interception.dll` missing from `dist\`, or driver not installed (step K), or wrong-arch lib (must be `x64\`). |
| Driver installs, Device Manager Code 52 | Signing chain. Redo S (cert in **both** Root and TrustedPublisher) and T (testsigning + reboot, watermark visible). |
| `hydractl` says hydrad not reachable | Service not running: `Start-Service Hydra`, or foreground `hydrad.exe run` for debugging. |

## Appendix: full rollback (undo everything)

Elevated, in this order:

```powershell
.\hydractl.exe stop                                   # or Stop-Service Hydra
.\hydrad.exe uninstall
pnputil /delete-driver .\iddseat.inf /uninstall       # removes the display driver
bcdedit /set testsigning off                          # then reboot; watermark gone
# optional: remove the test cert from LocalMachine\Root and \TrustedPublisher (certlm.msc)
# optional: install-interception.exe /uninstall       # then reboot
```

RDP-Wrapper is untouched by any of this and keeps working exactly as before.
