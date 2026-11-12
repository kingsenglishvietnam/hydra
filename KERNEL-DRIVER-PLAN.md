# KERNEL-DRIVER-PLAN.md — replacing RDP with a WDDM miniport

Written 2026-11-12 after run B. Supersedes the mode 4 provider approach.

## Decision

Mode 4's provider path is closed: with the cap lifted and the IDD working,
termsrv disconnects 4 ms after IddCxMonitorArrival (see MODE4-RESULTS.md).
That is termsrv enforcing a contract the provider cannot satisfy without
integrating input into the same package. Three paths forward:

| path | what | verdict |
|---|---|---|
| A. WDDM kernel-mode miniport | true DISPLAY class driver, swapchain -> network encoder | RECOMMENDED |
| B. Kernel protocol + UMDF IDD | move protocol to kernel, keep IDD | still depends on termsrv APIs |
| C. Mirror driver | W2000-era kernel mirror | dead (removed 10 2004) |

Path A removes termsrv from the loop entirely. That is the plan.

## What to reuse

- Network transport and encoder (only the pixel source moves)
- EDID generation (same monitor descriptor logic)
- Safety-gate + undo discipline (more important, not less: kernel driver
  bugs are system-wide bugchecks)

## What to discard

- IWRdsWddmIddProps callbacks (OnDriverLoad, GetInputHandles, GetHardwareId)
- HydraProto listener registration
- termsrv-guard dependency (keep the tooling; the miniport does not need
  the patch)

## Steps

1. Install WDK matching SDK 26100. Confirm km headers are present.
   Samples come from github.com/microsoft/Windows-driver-samples (not
   bundled with the WDK installer).
2. Build and install the unmodified KMDOD sample under test-signing.
   Verify Device Manager shows a new display adapter. Proves toolchain
   and signing before any code is written.
3. Add a log line to KMDOD's DxgkDdiStartDevice writing to
   C:\HydraLog\kmdod.log. Kernel logging without a debugger.
4. Implement frame capture in DxgkDdiPresent: copy surface to a buffer,
   increment a counter in the log. No network yet.
5. Wire the buffer to the existing encoder/transport. Kernel-mode remote
   display now works.
6. Add input injection via a HID source driver or filter driver fed by
   the network listener.
7. Safety-gate before every kernel-driver load. Failed gate ends the
   session, same as mode 4.

## Risks

- Kernel display driver bugs bugcheck the whole machine. Snapshot and
  recovery stick before every load, no exceptions.
- Start with the simplest possible miniport: one monitor, one mode
  (1920x1080x60), no power management, no multi-mon.
- DxgkDdiPresent and flip handling is the hardest piece. Swapchains,
  fences, DWM composition. KMDOD is the starting point, not a shortcut.

## Why this replaces RDP

The session model requires termsrv to own the display and input paths.
A WDDM miniport owns them directly. No session arbitration, no reason=18,
no GetInputHandles contract to satisfy. Cost: kernel stability. Benefit:
the stack is entirely ours.
