# Developing a four-user local setup

The intended layout is one Windows PC with four directly connected monitors
and four keyboard/mouse sets: console A and independent sessions B, C and D.
`examples/four-local-seats.toml` describes that layout. It is a **template**,
not a detected hardware map, an installer, or a claim of ASTER parity.

## Input fixes

- Each agent receives its configured seat name. Previously every agent's cursor
  publisher wrote into `Global\HydraSeat_B_pix`, even for C and D.
- The router resolves all physical input owners before enabling filters. Two
  identical keyboards have identical hardware IDs; picking the first matching
  seat silently joined their input. Ambiguous IDs, overlapping selectors and
  missing devices now fail configuration instead.
- Agent and injector ports are validated together. A seat at port 56789 also
  uses 57789; a second seat cannot claim that port.
- Names, monitors, numeric devices and explicit sessions cannot be reused.
  Multiple extra seats require explicit `user:NAME` or numeric sessions.
- A single `auto` seat waits when no unambiguous remote session exists; it no
  longer injects into the console as a fallback.
- Config parsing is transactional: a failed reload does not partially replace
  the previous configuration.

## Inspect without starting seats

After building, this command only reads and validates the configuration and
prints JSON. It does not require the Hydra service or Interception driver:

```powershell
.\dist\hydractl.exe config .\examples\four-local-seats.toml
```

With Interception already installed, the router can inspect assignments without
enabling capture, opening listener sockets or starting agents:

```powershell
.\dist\seat_router.exe --check --seat 56789 --kbd 2 --mouse 12 --seat 56790 --kbd 3 --mouse 13 --seat 56791 --kbd 4 --mouse 14
```

Exit 0 means the **current inventory** resolved uniquely. Exit 2 is invalid,
missing or ambiguous configuration; exit 3 means the driver is unavailable.
This is not an end-to-end test of sessions, displays or audio.

## Identical-model devices

`VID_413C&PID_2113`, for example, is a model ID, not a physical keyboard's unique
identity. If two connected devices report it, neither a longer identical ID nor
different capitalization separates them.

Explicit Interception numbers can distinguish those devices **within the
current boot**, after identifying each one interactively. They are not stable
USB-port assignments and must be rechecked after a reboot or reconnect. Never
copy the example's numbers into a live service without identifying the devices.
The router can verify existence and uniqueness, but cannot prove that a numeric
device is still on the intended person's desk.

Persistent per-instance/USB-port binding remains a development requirement.
The current Interception public API exposes model IDs, not that binding. This
change detects the problem; it does not pretend to solve persistent identity.

## Local client lifecycle

`hydra-local.ps1` is a new experimental, seat-scoped launcher. By default it
only prints a launch plan. It reads the config through the compiled parser and
matches FreeRDP's display IDs to Windows display rectangles, not model names or
assumed numbering. Identical monitors and negative desktop coordinates are
supported; ambiguous geometry is rejected.

Prerequisites for planning are `hydractl.exe`, `clip_console.exe`, and
`dist/freerdp/sdl-freerdp.exe`. For starting clients, the Hydra service, input
driver, separate local accounts and concurrent-session support must already be
configured. This launcher does not install those or change Windows policies.

```powershell
# Preview only (does not capture input or open a desktop):
.\hydra-local.ps1 -Seat B -Plan

# Only after configuring/testing the prerequisites:
.\hydra-local.ps1 -Seat B -Start
.\hydra-local.ps1 -Seat C -Start
.\hydra-local.ps1 -Seat D -Start

# Close ONLY the owned client for C; B/D and the shared service stay running:
.\hydra-local.ps1 -Seat C -StopClient
```

All client connections target this PC's loopback address. FreeRDP handles the
credential prompt; passwords are not saved or passed in argv. A start checks
the daemon's live seat map against the file. Close checks the recorded process
ID, start time, executable path and console session, retaining a process handle
to avoid a PID-reuse race. Closing a client **disconnects**, it does not log off
that Windows user or stop the shared input router. Normal session logout and
recovery from a stale RDP session remain separate operations.

`hydra7.ps1` remains the legacy single-secondary-seat launcher, which stops
clients by process name and stops the shared service. **Do not mix its start/stop
commands with multi-seat clients.**

The new launcher does not yet handle taskbar/virtual-desktop pinning or audio
assignment. FreeRDP's WinMM backend ignores device arguments and uses
`WAVE_MAPPER`; adding `dev:<number>` alone does not isolate audio. These remain
integration work. The new launch/close paths have planning and identity tests,
but have not been validated with four live, driver-isolated sessions.

## Tests

```powershell
.\tests\run-tests.ps1
# After building the production binaries and the Interception user-mode library:
.\tests\test_cli.ps1
```

The native tests exercise configuration conflicts, duplicate-model physical
inventories, numeric/model-ID overlap, independent cursor namespaces, automatic
session selection, local display matching, process identity and EDID generation.
Two inert child processes exercise the actual seat-scoped close path, including
a stale PID record. A test-only named pipe exercises multi-buffer replies in the
actual CLI. CLI tests exercise the compiled offline exporter and invalid-argument
paths without capturing input.

Hardware acceptance remains necessary: four simultaneous sessions with correct
input and audio, repeated login/logout, reconnects, sleep/resume, reboot, DPI and
display changes, and Windows updates. No performance or independent-lock-screen
equivalence to ASTER is claimed. Hydra's current local RDP presentation and the
Windows display/session boundary are still architectural limitations.
