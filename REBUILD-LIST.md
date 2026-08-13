# Rebuild list — switching the Hydra Shell from VS 2026 to VS 2022

Everything below was built with the **2026** toolset (`\18\Community`), because
the Hydra Shell shortcut calls its `vcvars64.bat` and every build script picks
up whatever `cl.exe` is on PATH.

The two exceptions are the provider builds, which were run tonight by calling
2022's MSBuild by full path — so those are already 2022, and the `.vcxproj`
pins its own toolset regardless of shell.

---

## 0. Switch the shortcut, then open a NEW shell

```powershell
Test-Path 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
```

```powershell
$p="$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Hydra\Hydra Shell.lnk"; $s=(New-Object -ComObject WScript.Shell).CreateShortcut($p); $s.Arguments = $s.Arguments.Replace('C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat','C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'); $s.Save(); $s.Arguments
```

Close every existing Hydra Shell. They keep the old PATH.

Verify in the new one:

```powershell
where.exe cl.exe
```

Must be under `2022\BuildTools`, not `18\Community`.

---

## 1. Unregister the provider first

A registered provider locks its DLL, and a staged driver should not be replaced
underneath a live listener.

```powershell
cd C:\Programs\hydra; .\rdsprov-register.ps1 -Unregister -Apply
```

---

## 2. Drivers — MSVC, affected by the shell

Delete the stale objects first, or a failed compile silently links the old one.
That exact trap hid three bugs until tonight.

```powershell
cd C:\Programs\hydra; Remove-Item dist\driver\*.obj, dist\driver-remote\*.obj, dist\driver\*.cat, dist\driver-remote\*.cat -Force -ErrorAction SilentlyContinue
```

```powershell
cd C:\Programs\hydra; .\build-driver.ps1
```

```powershell
cd C:\Programs\hydra; .\build-driver.ps1 -Remote
```

Confirm the two DLLs differ — identical hashes mean `-Remote` is not taking:

```powershell
cd C:\Programs\hydra; (Get-FileHash dist\driver\iddseat.dll).Hash; (Get-FileHash dist\driver-remote\iddseat.dll).Hash
```

**Also `hydrakbd`**, which is the other MSVC driver in the tree and equally
affected:

```powershell
cd C:\Programs\hydra; .\build-kbfilter.ps1
```

---

## 3. Catalog + signing — required after ANY driver rebuild

The `.cat` hashes the binaries. A rebuilt DLL with an old catalog fails to load
and says very little about why.

```powershell
cd C:\Programs\hydra; & 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\stampinf.exe' -f dist\driver-remote\iddseat-remote.inf -d * -a amd64 -v 1.0.0.1
```

```powershell
cd C:\Programs\hydra; & 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:dist\driver-remote /os:10_x64 /verbose
```

```powershell
cd C:\Programs\hydra; .\sign-driver.ps1 -DriverDir .\dist\driver-remote
```

Same three for `dist\driver` and for `dist\kbfilter` if you rebuilt those.

---

## 4. Re-stage the driver package

The old `oem84.inf` in the driver store still points at the 2026 binary.

```powershell
pnputil /enum-drivers | Out-String -Width 200 | Select-String -Pattern 'Hydra' -Context 3,3
```

Delete the old published name it reports, then add the new package:

```powershell
pnputil /delete-driver oem84.inf /uninstall
```

```powershell
cd C:\Programs\hydra; pnputil /add-driver dist\driver-remote\iddseat-remote.inf /install
```

---

## 5. NOT affected — no rebuild needed

- **`hydrardp.exe`** — built by `build-rdpclient.ps1` with **MinGW gcc** from
  MSYS2, not MSVC. The Hydra Shell's toolset is irrelevant to it.
- **`TestProtocol_Ext.dll`** — the provider. Its `.vcxproj` pins the toolset via
  `/p:PlatformToolset`, and tonight's builds already used 2022's MSBuild
  explicitly. If you rebuild it, keep passing `/p:PlatformToolset=v143` and do
  **not** pass `/p:SpectreMitigation=Spectre` — the 2022 Build Tools have only
  the plain ATL libs, while 2026 has only the spectre ones. The flag is
  required on 2026 and breaks the build on 2022.
- **`mirror.exe`, `hydractl.exe`, `hydrad.exe`, `seat_router`, `seatB_agent`,
  `audio_bridge`, `clip`** — MSVC, so technically affected, but they are
  ordinary user-mode executables with no driver signing or kernel interaction.
  A toolset change is very unlikely to matter. Rebuild them only if something
  starts misbehaving.

  ```powershell
  cd C:\Programs\hydra; .\build.ps1
  ```

---

## 6. Re-register and retest

```powershell
cd C:\Programs\hydra; .\rdsprov-register.ps1 -Register -Apply
```

Then credentials, then trigger — see `RESUME-2026-08-11d.md`.

---

## Worth saying

There is no evidence yet that the 2026 toolset is the problem. The driver has
never loaded under **either** compiler, and the instrumented log shows the
failure is at driver load, not anywhere the compiler would plausibly reach.

Test-signing has been set but never activated — that reboot has not happened.
Doing the toolset switch first means changing two variables at once and not
knowing which mattered.

**Reboot and test under 2026 first.** If it loads, the toolset question is
settled by evidence. If it does not, switch and you have a clean comparison.

---

## SUPERSEDED 2026-08-13 — the switch happened by force

The reset removed VS 2026 (`18\Community`) entirely. `install-shortcut.ps1`
rediscovered 2022 BuildTools, so the Hydra Shell now points there and every
build picks up 2022's cl.exe. Sections 0 and 5's toolset discussion are history.

The clean comparison this file argued for is now unavailable in one direction
and free in the other: there is only 2022. If the driver loads under it, the
toolset mattered. If 0xD000000D persists, it never did.

Also settled since: testsigning IS active and the HydraTest cert is trusted in
both LocalMachine\Root and TrustedPublisher — so 'test-signing never activated'
is no longer an open variable. The blocker is measured under valid conditions.

Still required before any of this runs: WDK 10.0.28000.0, which the reset took.
See REBUILD.md section 11.
