# Hydra — build & install guide

This is the operational companion to `ARCHITECTURE.md`. It covers building every
component, the one hard gate (a test-signed display driver), and the install/run
order. Read the "status" table at the bottom first if you want to know exactly
what is compile-verified versus what builds on your machine.

---

## Components

| Component        | Language        | Builds with        | Runs in            |
|------------------|-----------------|--------------------|--------------------|
| `seat_router`    | C (Win32)       | cl **or** MinGW    | console session    |
| `seatB_agent`    | C (Win32)       | cl **or** MinGW    | each seat session  |
| `clip_console`   | C (Win32)       | cl **or** MinGW    | console session    |
| `respawn`        | C (Win32)       | cl **or** MinGW    | (standalone helper)|
| `hydractl`       | C++ (Win32)     | cl **or** MinGW    | anywhere           |
| `hydrad`         | C++ (Win32+SDK) | cl (Windows SDK)   | service (session 0)|
| `mirror`         | C++ (D3D11)     | cl (Windows SDK)   | console session    |
| `iddseat.dll`    | C++ (UMDF/IddCx)| **WDK** + MSBuild  | Wudfhost (UMDF)    |

Everything except `iddseat.dll` is built by `build.ps1`. The driver needs the WDK
and a signed catalog — its own section below.

---

## Prerequisites

- **Visual Studio 2022** with "Desktop development with C++" (gives `cl`, `link`,
  the Windows 10/11 SDK). Everything but the driver builds with just this.
- **WDK** matching your SDK version, plus the **"Windows Driver Kit" VS
  extension**, to build `iddseat.dll`. This also provides `stampinf`, `inf2cat`,
  `signtool`, and `makecert`/`pvk2pfx` (or use `New-SelfSignedCertificate`).
- **Interception SDK** (`interception.h` + `interception.lib`) for `seat_router`.
  Put the header on `INCLUDE` and the lib on `LIB`, or drop both beside
  `input\seat_router.c`. The interception driver (`install-interception.exe /install`)
  must also be installed on the target machine for input capture to work.
- **RDP-Wrapper** (or equivalent) installed and configured to allow the extra
  concurrent session(s). Hydra does **not** create sessions — Wall 1 in the
  architecture doc — it drives sessions RDP-Wrapper makes possible.
- For the driver: the machine must be in **test-signing mode** (dev) or the
  package must be **EV + attestation signed** (shipping). There is no unsigned
  load path for a Display-class driver.

---

## 1. Build the app components

From an **x64 Native Tools Command Prompt for VS**:

```
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Output lands in `.\dist` (the seven `.exe`s plus `seats.toml` and `iddseat.inf`).
If `seat_router` fails to compile, the Interception SDK isn't visible — fix the
include/lib path and re-run. The other three C programs and all three C++ pieces
have no external dependency beyond the Windows SDK.

> The four C programs also cross-compile with MinGW (`x86_64-w64-mingw32-gcc -O2`)
> if you want to smoke-test them off a Windows box — that's how the input stack
> was verified. `hydrad`/`mirror` are SDK/D3D and need `cl`.

## 2. Build the driver (`iddseat.dll`)

The reliable path is a **WDK "User Mode Driver (UMDF V2)" project** in Visual
Studio:

1. New Project → *User Mode Driver (UMDF V2)*. Add `iddseat/iddseat.cpp`,
   `iddseat/iddseat.h`, and the `common/` headers to it.
2. Project properties:
   - **Driver Model → IddCx**: add the IddCx headers/lib. Link `IddCx.lib` and
     the UMDF stubs (the template links `WdfDriverStubUm` automatically).
   - **C/C++ → Language → C++17**.
   - Point the packaging step at `iddseat/iddseat.inf`.
3. Build x64 Release. MSBuild runs `stampinf` (filling `DriverVer`) and produces
   `iddseat.dll` + `iddseat.cat` + the stamped `iddseat.inf` in the driver
   package folder.

<details>
<summary>Manual command-line build (no .vcxproj) — outline</summary>

```
:: compile (WDK include paths abbreviated as %UMINC% / %SDKINC%)
cl /c /EHsc /std:c++17 /I"%UMINC%" /I"%SDKINC%\um" /I"%SDKINC%\shared" ^
   iddseat\iddseat.cpp /Fo:iddseat.obj

:: link a UMDF DLL against IddCx + the UMDF stub
link /DLL /OUT:iddseat.dll iddseat.obj IddCx.lib WdfDriverStubUm.lib ^
     /LIBPATH:"%UMLIB%\x64"

:: stamp + catalog + sign (test cert)
stampinf -f iddseat\iddseat.inf -d * -v *
inf2cat /driver:. /os:10_X64
signtool sign /fd sha256 /a /s PrivateCertStore /n HydraTest iddseat.cat
```
Getting the exact WDK include/lib paths right by hand is fiddly; prefer the
.vcxproj unless you have a reason not to.
</details>

## 3. Test-sign and trust the catalog (dev machines)

```powershell
# one-time: create a self-signed code-signing cert
$cert = New-SelfSignedCertificate -Type CodeSigningCert `
  -Subject "CN=HydraTest" -CertStoreLocation Cert:\CurrentUser\My

# sign the driver catalog with it
& signtool sign /fd sha256 /a /sha1 $cert.Thumbprint .\dist\iddseat.cat

# trust it: install into LocalMachine Root + TrustedPublisher
$pw = ConvertTo-SecureString "hydra" -AsPlainText -Force
Export-PfxCertificate -Cert $cert -FilePath $env:TEMP\hydra.pfx -Password $pw | Out-Null
Import-PfxCertificate -FilePath $env:TEMP\hydra.pfx -Password $pw `
  -CertStoreLocation Cert:\LocalMachine\Root         | Out-Null
Import-PfxCertificate -FilePath $env:TEMP\hydra.pfx -Password $pw `
  -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
```

Enable test signing (reboot required):

```
bcdedit /set testsigning on
```

Copy `iddseat.dll`, `iddseat.cat`, and the stamped `iddseat.inf` into `.\dist`
alongside the exes.

---

## 4. Install and run

From `.\dist`, elevated:

```powershell
# 1. Install the virtual-display driver package (INF). This does NOT create a
#    monitor yet -- hydrad instantiates devices against it per seat.
pnputil /add-driver .\iddseat.inf /install

# 2. Install and start the control service.
.\hydrad.exe install
Start-Service Hydra          # or: sc.exe start Hydra
```

First-time configuration:

```powershell
# Discover input device numbers (opens a console on the physical console seat;
# press keys / move mice to see which number is which).
.\hydractl.exe learn

# Discover monitor device names.
.\mirror.exe                 # prints monitors + \\.\DISPLAYn names, then exits
```

Edit `seats.toml` (next to `hydrad.exe`, or `C:\ProgramData\Hydra\seats.toml`)
with the numbers/names you found, then:

```powershell
.\hydractl.exe reload
.\hydractl.exe status
```

`status` shows each virtual monitor and every supervised helper
(`router`, `clip`, `mirror:B`, `agent:B`, …) with running/waiting/stopped state.
Per-process stdout/stderr is logged under `C:\ProgramData\Hydra\logs\`.

Uninstall:

```powershell
.\hydractl.exe stop
.\hydrad.exe uninstall
pnputil /delete-driver .\iddseat.inf /uninstall   # removes the driver package
```

---

## How the pieces line up at runtime

```
hydrad (service, session 0)
├─ SwDeviceCreate  ─► iddseat.dll instance per seat ─► virtual monitor + shared surface
│                                                        (Global\HydraSeat_<seat>_surf)
├─ console session ─► seat_router.exe   (captures B/C input, routes over loopback)
│                  ─► clip_console.exe  (clamps seat A cursor to confine_monitor)
│                  ─► mirror.exe  x N   (opens each seat's shared surface, presents
│                                        it fullscreen on that seat's panel)
└─ each seat sess. ─► seatB_agent.exe   (receives routed input, SendInput into session)
```

The shared surface is opened **by name** across the session boundary
(`OpenSharedResourceByName`), so no handle duplication is needed; the presenter
builds its D3D device on the **LUID the driver publishes** in shared metadata, so
the copy is same-GPU/zero-copy. Frames use a single keyed-mutex key ("latest
frame wins") so a slow presenter never stalls the driver's swap-chain thread.

---

## Build/verification status — the honest version

**Compile-verified here (MinGW cross-compile, clean `-Wall -Wextra`):**
`seat_router.c`* , `seatB_agent.c`, `clip_console.c`, `respawn.c`.
(*router links against a minimal API-compatible Interception stub in this
environment; on your machine it links the real SDK.)

**Logic unit-tested natively (and, for EDID, cross-checked with `edid-decode`):**
- EDID generation/parse round-trip — `tests/test_edid.c` passes; the generated
  block validates as EDID 1.4 with the right manufacturer, resolution DTD, and
  checksum.
- `seats.toml` parser + `hydra_build_router_args` — `tests/test_config.cpp`
  passes, including literal-backslash device paths and the exact
  `"<kbd> <mouse> <port>"` triple string `seat_router` expects.
- The mode-string parser used by the driver (`WxH@Hz` → mode, with fallback).

**Compile- and link-verified with MSVC (x64, VS 2022) on the target machine:**
`hydractl.exe`, `mirror.exe`, `hydrad.exe`, plus `seatB_agent`, `clip_console`,
`respawn`. `seat_router` builds once the Interception SDK is present.

**Correct-shape, not yet compiled by anyone** (needs the WDK, which neither this
container nor a plain VS install provides):
- `iddseat.dll` — the IddCx driver. Structurally complete: per-seat EDID+mode,
  the three mode-enumeration callbacks, the shared-surface producer, and the
  adapter/monitor context threading are all filled in. Brace/scope-checked only.
  Expect first-build friction of the same kind `hydrad` had (missing CRT headers,
  wrong import lib) — that is normal, not a design problem.

**The genuinely experimental part** (called out in `ARCHITECTURE.md`, Risk #1):
binding a virtual monitor to a *specific TS session's* desktop so the mirror
shows that session and not the console's. hydrad drives helpers into the right
sessions, but which session actually renders into a given virtual monitor is the
part that needs real-hardware iteration. Everything else is deterministic; this
is where to expect to spend bring-up time.

**The hard gate:** the display driver must be test-signed and the machine in test
mode (§3). This is a Windows requirement, not a Hydra one — there is no unsigned
load path for a Display-class driver.

---

## Audio (do this, or seats go mute)

RDP redirects session audio to the connected client by default. Two problems:
the RDP audio channel buffers ~100-200ms+ behind the display (lip-sync drift you
can hear), and in the Hydra endgame there is **no RDP client connected** -- the
virtual monitor + mirror replace mstsc -- so redirected audio has nowhere to go
and the seat is simply silent.

Fix: make each seat session play audio **on the host**.

- Per-connection (during setup, while you still use mstsc): *Show Options ->
  Local Resources -> Remote audio -> Settings -> "Play on remote computer"*, or
  put `audiomode:i:1` in the seat's `.rdp` file.
- Machine-wide (recommended once seats are real): Group Policy ->
  *Computer Configuration -> Administrative Templates -> Windows Components ->
  Remote Desktop Services -> Remote Desktop Session Host -> Device and Resource
  Redirection -> "Allow audio and video playback redirection"* = **Disabled**.
  This forces host playback regardless of what any client asks for.

Per-seat audio devices come for free: the default output device is a per-user
setting, and seats run as distinct users. Plug in one USB headset per seat and
have each seat's user select it as default inside their own session.

---

## Troubleshooting

- **`build.ps1` reports FAILED for `seat_router.c`.** Expected until you supply the
  Interception SDK: drop `interception.h` and `interception.lib` into `input\`
  (or put them on `INCLUDE`/`LIB`) and re-run. Nothing else depends on it.
- **`cl` not found.** Either run from an *x64 Native Tools Command Prompt for VS
  2022*, or just run `build.ps1` from any prompt — it self-arms the VS x64 dev
  shell via `vswhere` + `Enter-VsDevShell`.
- **`Join-Path : Cannot bind argument to parameter 'Path' because it is null`.**
  Old script. `vswhere -latest` returns *nothing* (not an error) when no instance
  matches, and it hides Build Tools installs unless you pass `-products *`. The
  current script passes `-products *`, requires the x64 C++ toolset component, and
  null-checks the result.
- **Link error: unresolved `DEVPKEY_Hydra_SeatName`.** You are building against an
  older `hydra_devprops.h`. The current one emits the key objects itself
  (`DECLSPEC_SELECTANY`) rather than relying on `INITGUID`.
- **`LNK2019: unresolved external symbol SwDeviceCreate` (or `SwDeviceClose`).**
  The Software Device API lives in `cfgmgr32.dll`, but its exports are *not* in
  `cfgmgr32.lib`. Link **`onecore.lib`** instead. (Verified on a real SDK; the docs
  page for `SwDeviceCreate` is unhelpfully vague, and Chromium's virtual-display
  controller links the same way.)
- **`d3d11.h` / `swdevice.h` not found.** The Windows SDK component isn't installed
  with the C++ workload. Check `$env:WindowsSdkDir` inside the dev shell.
- **Driver won't load / Code 52.** Test signing isn't on, or the catalog isn't
  trusted. Re-check §3, and confirm `bcdedit` shows `testsigning Yes` after reboot.
