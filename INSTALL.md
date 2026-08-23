# INSTALL.md — from nothing to two working seats

Written for someone who has never seen this project. Assumes you can copy and
paste into PowerShell and nothing more.

**Read the warning at the bottom of `README.md` first.** This installs kernel
input filters and changes how Windows handles sessions. It has broken the
machine it was developed on, twice.

Budget an afternoon. Most of it is waiting for installers.

---

## 0. What you need before starting

**Hardware**

- A Windows 11 PC (developed on 24H2, build 26100)
- A second monitor with its own audio — HDMI or DisplayPort, so sound goes to
  the monitor rather than the PC's speakers
- A second keyboard and mouse, ideally wireless with their own USB receiver

**Windows**

- Windows 11 Pro or Home
- An administrator account
- **Smart App Control OFF** — see step 1. This is permanent and cannot be undone
  without reinstalling Windows.

**Licensing, read this**

Hydra uses RDP-Wrapper to allow two simultaneous sessions on a client SKU of
Windows. **Whether that is permitted by your Windows licence is your
responsibility.** This project was built for a single-user machine where both
seats are the same person. Your situation may be different. Check before
deploying it anywhere that matters.

---

## 1. Turn off Smart App Control

Smart App Control blocks unsigned executables, and everything here is unsigned.
It will block Hydra silently with *"An Application Control policy has blocked
this file"*.

Check first:

```powershell
Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard | Select-Object UsermodeCodeIntegrityPolicyEnforcementStatus
```

- `0` — off, nothing to do
- `1` — evaluation mode
- `2` — enforcing, must be turned off

**Settings → Privacy & security → Windows Security → App & browser control →
Smart App Control → Off.**

**This is permanent.** Smart App Control only enables itself on a clean Windows
install; once off, it cannot be switched back on. Defender is unaffected and
stays fully active.

If the Windows Security window will not open or lands off-screen:

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name VerifiedAndReputablePolicyState -Value 0 -Type DWord
```

Reboot, then re-check with the command above. It should read `0`.

---

## 2. Create the seat account

The second seat runs as its own Windows user. These instructions call it
`teacher`; use whatever you like and substitute throughout.

**Settings → Accounts → Other users → Add account → I don't have this person's
sign-in information → Add a user without a Microsoft account.**

Make it a **standard** user, not an administrator. Give it a password you will
remember — you will type it at every launch.

**Sign into it once** before continuing. Windows needs to build the profile, and
the first login takes several minutes. Then sign out (not just lock).

```powershell
Get-LocalUser teacher | Select-Object Name, Enabled
```

---

## 3. Install the build tools

**Visual Studio 2022 Build Tools** — compiles the C and C++.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended"
```

Fifteen to thirty minutes. Use `--passive`, not `--quiet`.

**Git**

```powershell
winget install --id Git.Git -e
```

**MSYS2** — provides the FreeRDP client.

```powershell
winget install --id MSYS2.MSYS2 -e
```

MSYS2 refuses to install over an existing `C:\msys64`. Rename any old one first.

Then in the **MSYS2 MinGW 64-bit** shell, not PowerShell:

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-freerdp
```

**PowerShell 7** — the launchers need it.

```powershell
winget install --id Microsoft.PowerShell -e
```

**Close and reopen your terminal** so the new tools are on PATH.

---

## 4. Install the third-party components

Each has its own licence. Read `THIRD-PARTY.md`.

### Interception — input capture

Download from https://github.com/oblitum/Interception (latest release) and
extract to `C:\Programs\Interception`.

```powershell
& "C:\Programs\Interception\command line installer\install-interception.exe" /install
```

**Reboot.** The driver only takes effect after one.

**Note the path has spaces**, so the quotes matter.

**Interception is dual-licensed:** free under LGPL for non-commercial use, paid
for commercial. If you are deploying this in a business, contact the author.

### RDP-Wrapper — two sessions at once

Download from https://github.com/stascorp/rdpwrap (latest release).

Defender flags it, which is expected — it patches Terminal Services:

```powershell
Add-MpPreference -ExclusionPath 'C:\Program Files\RDP Wrapper'
```

Run `install.bat` as administrator.

Then replace the bundled `rdpwrap.ini` with the current one from
https://github.com/sebaxakerhtc/rdpwrap.ini — the original has not been updated
for recent Windows builds and RDP-Wrapper will not work without a matching one.

**Then the single most important command in this guide:**

```powershell
sc.exe config TermService type= own
```

Without it RDP-Wrapper's `ServiceDll` never loads, you get one session only, and
the symptom is `ERRCONNECT_ACTIVATION_TIMEOUT` — which looks like a network
fault and is not. A Windows reset silently undoes this.

**Reboot**, then verify:

```powershell
sc.exe qc TermService | Select-String 'TYPE'
```

Must read `TYPE : 10 WIN32_OWN_PROCESS`.

```powershell
$p=(Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId; (Get-Process -Id $p -Module).ModuleName | Where-Object { $_ -match 'rdpwrap|termsrv' }
```

Both names must appear.

---

## 5. Get Hydra and build it

```powershell
git clone https://github.com/kingsenglishvietnam/hydra.git C:\Programs\hydra
cd C:\Programs\hydra
```

Copy the Interception SDK files the build needs — they are not redistributed
here:

```powershell
Copy-Item "C:\Programs\Interception\library\interception.h" C:\Programs\hydra\input\ -Force
Copy-Item "C:\Programs\Interception\library\x64\interception.lib" C:\Programs\hydra\input\ -Force
```

Fetch the FreeRDP binaries from your MSYS2 install:

```powershell
.\vendor-freerdp.ps1
```

Build:

```powershell
.\build.ps1
```

Use the **x64 Native Tools Command Prompt for VS 2022**, then type `pwsh` — the
compiler needs its environment. `install-shortcut.ps1` creates a "Hydra Shell"
shortcut that does both.

```powershell
Get-ChildItem C:\Programs\hydra\dist\*.exe | Select-Object Name
```

Expect `hydrad`, `hydractl`, `seat_router`, `seatB_agent`, `session_capture`,
`mirror`, `audio_bridge`, `route_endpoint`, `clip_console` and others.

---

## 6. Configure your machine's specifics

`seats.toml` needs three things that are different on every machine.

### Your input device hardware IDs

```powershell
Stop-Service Hydra -EA SilentlyContinue
.\dist\seat_router.exe --learn
```

Press keys on the **seat's** keyboard and move the **seat's** mouse. It prints a
`hwid:` line for each. Ctrl+C when done.

Copy the distinctive part — for example `VID_1EA7&PID_0066` — into `seats.toml`.

**Match by hardware ID, never by device number.** Interception's numbers change
on every boot.

### Your monitor's audio endpoint

```powershell
.\dist\route_endpoint.exe --list
```

Take the first segment of the GUID for your **monitor's** audio — for example
`3012a3df` from `{0.0.0.00000000}.{3012a3df-...}` — and set `audio_bridge` in
`seats.toml`.

**A Windows reset reissues these**, so if audio stops working after one, this is
why.

### The display mode

```
display_mode = "off"       # mode 7 -- start here
```

Then deploy the config:

```powershell
.\setup.ps1
```

That copies `seats.toml` into `dist\` and installs the Hydra service as
Manual-start.

---

## 7. Arrange your monitors

**Settings → System → Display.** Drag the seat's monitor well away from your
own — far to the right, or above with only a corner touching.

Windows lets your cursor cross wherever the edges meet, so a large shared edge
means wandering onto the student's screen constantly.

```powershell
.\dist\clip_console.exe
```

That reports true pixel coordinates. **Do not use
`[System.Windows.Forms.Screen]`** — it is not DPI-aware and reports scaled sizes
that are simply wrong.

---

## 8. First run

```powershell
.\hydra7.ps1
```

A window opens asking for the seat account's password. **Echo is off** — a typo
shows as `ERRCONNECT_LOGON_FAILURE`, not as a wrong-password message.

After logging in you should have:

- the seat's desktop full-screen on its monitor
- the seat's keyboard and mouse driving only that screen
- your own keyboard and mouse unaffected
- the seat's audio from the monitor's speakers

```powershell
.\dist\hydractl.exe status
```

Expect `router`, `agent:B`, `abcap:B`, `abren:B` — and **no `capture:B`** in
mode 7.

To stop:

```powershell
.\hydra7.ps1 -Stop
```

**If anything goes wrong**, type this and press Enter — it works even if you
cannot see the screen, and releases the captured input in about two seconds:

```powershell
Stop-Service Hydra
```

---

## 9. Set up your safety net — do this before anything else breaks

### A bootable recovery drive

**This is not optional if you intend to touch drivers.** A bad driver install
produced a boot loop here that cost an OS reinstall, and the logs that would
have explained it were destroyed by the reset.

Plug in a USB stick of 16 GB or more. Search Windows for **"Create a recovery
drive"**, tick **"Back up system files"**, and let it run — it takes half an
hour or more.

**Then boot from it once.** An untested recovery drive is not a recovery drive.

### Back up everything a reset would destroy

```powershell
.\hydra-backup.ps1
```

That puts on the stick: the whole git history as one verifiable bundle, the
built binaries, the third-party trees, the documentation, and `MACHINE-STATE.txt`
— a record of every machine-specific setting a Windows reset silently reverts.

Run it again whenever you change something that works.

```powershell
Get-Content "E:\hydra-backup\MACHINE-STATE.txt" | Select-Object -First 10
```

Adjust the drive letter. Read it once now, so you know what it looks like before
you need it at seven in the morning.

### Before any driver work

```powershell
.\safety-gate.ps1 -Label "what-you-are-about-to-do"
```

It verifies three levels of undo and refuses if any is missing. It exists because
skipping it cost a working machine, twice.

---

## 10. Day-to-day

**Start:**

```powershell
cd C:\Programs\hydra
.\hydra7.ps1
```

**Stop:**

```powershell
.\hydra7.ps1 -Stop
```

**Panic:** type `Stop-Service Hydra` and press Enter. Works blind.

**A second mode is available** — `.\hydra6.ps1` — which routes the seat through
a virtual display and a mirror. It costs more than twice the CPU and looks
slightly worse, but it is the shape needed for a *third* seat and a useful
fallback if mode 7 misbehaves. `MODES.md` explains all seven.

**Switching modes needs `display_mode` changed** in `seats.toml` and
`.\setup.ps1` run. Mode 7 wants `"off"`, mode 6 wants `"capture"`. Getting this
wrong is the most common cause of a confusing failure.

---

## 11. When it does not work

`MODES.md` has a fuller table. The ones you are most likely to hit:

| symptom | cause |
|---|---|
| `ERRCONNECT_ACTIVATION_TIMEOUT` | `sc qc TermService` must read `WIN32_OWN_PROCESS`. Step 4. |
| "An Application Control policy has blocked this file" | Smart App Control. Step 1. |
| `ERRCONNECT_LOGON_FAILURE` | Wrong password. Echo is off; retype it carefully. |
| Seat's keyboard does nothing | Interception was installed but not rebooted, or the receiver was plugged in after boot — Interception binds devices at boot. Reboot with everything connected. |
| No sound from the monitor | `audio_bridge` GUID in `seats.toml` does not match `route_endpoint.exe --list`. |
| Client opens on the wrong screen | Monitor indices change between boots. The launchers detect it; a hand-typed command does not. |
| Both seats lock together | Expected. Locking the console switches Windows to the secure desktop machine-wide. Lock the seat instead. |
| A launcher dies on a progress message | PowerShell 7.4 changed how native stderr is treated. All launchers set `$PSNativeCommandUseErrorActionPreference = $false`; a new script needs it too. |

---

## 12. Things that will break it

- **Installing Logitech software.** Installing a Logitech mouse driver *inside
  the seat session* tears down that session's display stack. It is the confirmed
  cause of a long-standing capture failure. If Windows offers it, decline:

```powershell
Set-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching' -Name SearchOrderConfig -Value 0 -Type DWord
```

- **Locking the console mid-lesson.** Takes the student's screen with it.
- **Leaving a seat session disconnected** rather than logged off. RDP-Wrapper
  holds it until a reboot.
- **A Windows reset or feature update.** Reverts `type= own`, the audio endpoint
  GUIDs, Interception's filters and Smart App Control. `MACHINE-STATE.txt` on
  your recovery stick is the record of what to put back; `REBUILD.md` is the
  procedure.

---

## What to expect from it

**Good:** documents, browsing, slides, typing, anything static. The seat feels
like a normal Windows machine.

**Poor:** video. Around 25–28 fps and roughly half the CPU of a four-core
laptop, because a loopback RDP session composes its desktop in software — Windows
only gives a session GPU composition when a display is bound to it, which is not
reachable on a client SKU.

Play video on the console seat. If you need a video-capable second seat, look at
ASTER or Linux multiseat; `MODES.md` explains why and compares them honestly.

---

## Where to go next

- **`MODES.md`** — all seven modes, every failure with its cause. The most
  useful document here.
- **`REBUILD.md`** — rebuilding the machine from scratch after a reset.
- **`FUTURE.md`** — what is unfinished and what might be worth doing.
- **`INCIDENT-2026-08-12.md`** — how a driver install cost an OS reinstall, and
  what to do differently.
- **`THIRD-PARTY.md`** — what Hydra depends on and under what terms.
