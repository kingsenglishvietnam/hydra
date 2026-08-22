# THIRD-PARTY.md

Hydra is licensed under **AGPL-3.0** (see `LICENSE`). That covers the code in
this repository — the C, C++ and PowerShell written for this project.

**No third-party binaries or source are included here.** `.gitignore` excludes
`dist/`, `*.exe`, `*.dll`, `*.lib`, `*.sys` and the vendored Interception
headers. Everything below has to be obtained separately, and each carries its own
licence which you must read and comply with.

This file lists what Hydra depends on and where it comes from. **Licences are
noted where known; verify each one yourself before redistributing anything.**
Some of these are kernel-level components on Windows and the terms matter.

---

## Required

### FreeRDP
The RDP client. `sdl-freerdp.exe` and its DLLs are the pixel and audio path for
modes 1–3, 6 and 7.

- Source: https://github.com/FreeRDP/FreeRDP
- Obtained via: MSYS2's `mingw-w64-x86_64-freerdp` package, then copied into
  `dist\freerdp\` by `vendor-freerdp.ps1`
- Licence: Apache-2.0 (verify — FreeRDP has components under other terms)

**Note:** the MSYS2 build is compiled `WITH_VAAPI_H264_ENCODING=ON`. VA-API is a
Linux path and `/gfx:h264` crashes on Windows. Use `/gfx:rfxc`.

### Interception
Kernel-level input capture. `seat_router` uses it to take the wireless keyboard
and mouse away from the console and route them to the seat.

- Source: https://github.com/oblitum/Interception
- Obtained from its own installer, into `C:\Programs\Interception`
- `interception.h` and `interception.lib` are restored by hand during a rebuild;
  they are not ours to redistribute
- Licence: **verify before redistributing** — this is a kernel driver

### RDP-Wrapper
Allows more than one concurrent session on a client SKU of Windows.

- Source: https://github.com/stascorp/rdpwrap
- The `rdpwrap.ini` that matches current Windows builds comes from
  https://github.com/sebaxakerhtc/rdpwrap.ini
- Licence: Apache-2.0 (verify)

**This is the component with the clearest licensing question for end users.**
Running two interactive sessions on a client SKU of Windows may not be permitted
by your Windows licence. Hydra was built for a single-user machine where both
seats are the same person. **Check your own Windows licensing before deploying
it.**

### Virtual Display Driver (MttVDD)
Provides the virtual display mode 6 parks its client on.

- Source: https://github.com/VirtualDrivers/Virtual-Display-Driver
- Use the **shipped release**, signed by SignPath Foundation — it installs
  without test-signing. A from-source build did not load here.
- Licence: verify at the repository

Only needed for mode 6. Mode 7 does not use it.

---

## Optional / development only

### Microsoft WDK IddCx sample
Read as a reference while debugging `iddseat.cpp`. Three bugs were found by
diffing against it. Not redistributed here.

- Source: https://github.com/microsoft/Windows-driver-samples, `video/IndirectDisplay`
- Licence: MIT

### Microsoft RDS protocol provider sample (`TestProtocol_Ext`)
The basis for `HydraProto`. Mode 4 is closed, so this is historical.

- Ships with the Windows SDK / RDS samples
- Licence: MIT (verify)

### VirtualDesktop PowerShell module
Used by the launchers to pin the client window across virtual desktops.

- Source: PowerShell Gallery, `VirtualDesktop` by MScholtes
- Licence: MIT (verify)

### Windows Driver Kit tools
`stampinf`, `Inf2Cat`, `devcon`, `devgen`, `WdfVerifier`. Used to build and
stage the IddCx drivers in `iddseat/`. Microsoft licensing applies; not
redistributed.

### Sysinternals
`PsExec` for removing phantom devnodes as SYSTEM, `DebugView` for reading
`OutputDebugString` from a driver running under a restricted token.

- Source: https://learn.microsoft.com/sysinternals
- Microsoft licensing applies

---

## What is in this repository

Everything under this tree that is not listed above:

- `input/` — `seat_router`, `seatB_agent`: Interception-based input routing
- `capture/` — `session_capture`: Desktop Duplication into a shared ring
- `mirror/` — `mirror`: DXGI presentation of the ring
- `rdp/` — `hydrardp`: a headless FreeRDP client publishing to the ring
- `audio/` — `audio_bridge`, `route_endpoint`: per-seat audio
- `hydrad/` — the service that supervises the helpers
- `iddseat/` — an IddCx indirect display driver (modes 4 and 5)
- `common/` — the shared-memory IPC layout
- `*.ps1` — build, launch, test and recovery scripts
- `*.md` — documentation

All AGPL-3.0.

---

## A note on what AGPL means here

You may use Hydra for anything, including commercially and inside a company or
school, with no obligation to publish anything.

If you **distribute** a modified version, or offer it over a network, you must
publish your complete source under AGPL-3.0 as well.

In short: use it freely, but you cannot close it.

---

## No warranty

Hydra manipulates kernel input filters, installs display drivers, and modifies
Terminal Services configuration. **It has broken the machine it was developed
on, twice** — see `INCIDENT-2026-08-12.md`, which cost an OS reinstall.

`safety-gate.ps1` exists because of that, and requires a tested bootable recovery
stick before any driver operation. Use it.

This software is provided without warranty of any kind, as stated in the AGPL.
Read `MODES.md` and `REBUILD.md` before running anything.


---

## IMPORTANT: Interception is dual-licensed

Interception is LGPL for NON-COMMERCIAL use, with explicit permission to
distribute its drivers and installers so long as your code talks to the driver
only through the library API -- which is how seat_router uses it.

For COMMERCIAL use it requires one of two paid licences from the author,
francisco@oblita.com. See the licenses/ directory in the Interception
repository.

SO: Hydra is AGPL-3.0 and you may use IT commercially. But Hydra depends on
Interception for input isolation, and a commercial deployment would need a
commercial Interception licence separately. That is between you and the
Interception author; it is not something this project can grant.

Non-commercial use -- a school, a home, personal use -- is covered by the LGPL
terms at no cost.

Source: https://github.com/oblitum/Interception


