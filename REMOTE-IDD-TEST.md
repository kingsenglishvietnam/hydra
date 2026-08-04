# Remote-session IDD — the decisive test (Path A)

## The question this settles

The console IDD (what we built and shipped) attaches its virtual monitor to the
**console session** — so it shows *your* desktop as an extended panel, not
teacher's session. That's console-IDD behaviour working as designed.

Microsoft's documented mechanism for a per-**session** virtual monitor is a
**remote-session IDD**: set `IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER` (0x4) and
`IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE` (0x1) in the adapter caps. But there's a
hard constraint that this test exists to probe:

> The OS **fails `IddCxAdapterInitAsync`** if the driver sets
> `REMOTE_SESSION_DRIVER` for a device **that was not created by the OS remote-
> desktop stack**. (A remote IDD cannot be a console IDD, and vice versa.)

And a remote IDD is **not** instantiated by `hydrad`'s `SwDeviceCreate` — it is
loaded by the OS's remote-desktop/graphics stack when a session connects.

**The open, machine-specific question:** does **RDP-Wrapper** trigger that remote-
IDD load path? RDPWrap patches `termsrv.dll` to permit concurrent *sessions*; it
is not known to hook the WDDM/graphics stack that instantiates remote IDDs. If it
doesn't, the remote IDD will **never load** on this box:
`SwDeviceCreate` is forbidden to load it, and RDPWrap's session may not invoke the
OS remote-display stack that's supposed to. That is the thing this test resolves —
empirically, not by reasoning.

## What the build switch does

`iddseat.cpp` now has a compile-time switch (same code, one define):
- default → `caps.Flags = NONE` (console IDD — the working version)
- `-Remote` → `caps.Flags = REMOTE_SESSION_DRIVER | USE_SMALLEST_MODE`

Build the experimental variant:
```powershell
.\build-driver.ps1 -Remote
```

## Test protocol (do this fresh, ~30 min)

1. **Preserve the working driver.** The remote build overwrites
   `dist\driver\iddseat.dll`. Copy the current (console) one aside first:
   ```powershell
   Copy-Item dist\driver\iddseat.dll dist\driver\iddseat-console.dll
   ```
2. **Also set the INF directive** the docs require for remote IDDs, in
   `iddseat.inf` under the UMDF service section:
   ```
   UmdfHostProcessSharing = ProcessSharingDisabled
   ```
   (and remove any `DeviceGroupId`). Re-stamp after editing.
3. Build remote: `.\build-driver.ps1 -Remote`
4. Stamp + sign + install as before (`stampinf`, `sign-driver.ps1`,
   `pnputil /add-driver ... /install`).
5. **The measurement.** Try to bring the device up two ways and watch which (if
   either) succeeds:
   - via `hydrad` (`SwDeviceCreate`) — **expected to FAIL** `IddCxAdapterInitAsync`
     (device not created by the RDP stack). Check Event Viewer →
     Applications and Services → Microsoft → Windows → Iddcx / DeviceSetupManager,
     and the driver's own `DbgPrintEx` (DebugView, kernel capture).
   - by connecting teacher via RDP-Wrapper — **the real question**: does a remote
     IDD instance load for that session? Watch the same logs on connect.

## Decision matrix

- **Remote IDD loads on RDPWrap connect** → Path 1 is viable. Pursue it fully:
  restructure instantiation so the RDP stack (not `hydrad`) owns the device, set
  the session display config, composite frames. This is the *correct* Hydra
  architecture and dissolves the session-binding problem entirely.
- **Remote IDD never loads (init rejected / no instance on connect)** → Path 1 is
  a dead end **on RDP-Wrapper specifically** (would work on Server/AVD-class
  Windows). Pivot to **Path B (session capture)**: drop the virtual monitor
  entirely; have `mirror` capture teacher's session framebuffer (Desktop
  Duplication API — GPU-to-GPU, ~1–2 ms/frame, not slow) and present it on the
  panel, compositing the cursor from `IddCxMonitorQueryHardwareCursor2`-style
  position queries or `GetCursorInfo`. This fits wrapped-consumer-Windows
  multiseat and sidesteps the IDD session-binding constraint.

## Revert to the working console driver

If the experiment fails and you want the known-good version back:
```powershell
Copy-Item dist\driver\iddseat-console.dll dist\driver\iddseat.dll
# re-stamp/sign/install, or just rebuild without -Remote:
.\build-driver.ps1
```

## Status of this finding

The mutual-exclusivity and the `SwDeviceCreate`-can't-load-remote facts are from
Microsoft docs (IddCx 1.4 remote-IDD updates, `IDDCX_ADAPTER_FLAGS`). Whether
RDP-Wrapper triggers remote-IDD loading is **not** documented either way — it is
the empirical unknown this test closes. Treat the result as the branch point for
the entire display half of the project.
