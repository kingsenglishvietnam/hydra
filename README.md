# Hydra

**Two seats, one Windows machine.** A second person gets their own monitor,
keyboard, mouse and audio, working in their own Windows session, on hardware
that only came with one.

Built for a classroom in Ho Chi Minh City, to get two working seats out of one
machine at a single table.

**Status: working, and in active development.** It has run real lessons, and
October 2026 brought H.264 video on the iGPU and the seat's own pointer drawn
over its panel. Work continues — [`ROADMAP.md`](ROADMAP.md) has what's next.
Published so anyone who wants a second seat on a Windows machine has somewhere
to start.

---

## What it does

```
   Surface Book 3, one Windows install
   │
   ├── console session   →  laptop panel   +  wired keyboard/mouse
   └── seat B session    →  external 2770  +  wireless keyboard/mouse  +  monitor audio
```

Seat B is a genuine second Windows session — its own desktop, its own logged-in
user, its own programs. Not a shared screen, not a remote view.

**Input** is captured at kernel level by Interception, matched by USB hardware
ID, and injected into the seat's session. The two seats never see each other's
keystrokes.

**Display** comes from a loopback RDP client running fullscreen on the seat's
own monitor: mstsc, with the seat's desktop drawn on the iGPU and encoded to
H.264 by Quick Sync (`hydra7-avc.ps1`).

**Cursor** is drawn by Hydra itself. An RDP client never learns where the seat's
pointer is, so the seat's agent publishes its position and the seat's own pointer
files, and an overlay draws that pointer over the panel — at the size and colour
chosen in the seat's mouse settings.

**Audio** is routed per-seat, so the student's sound comes out of their monitor
and yours does not.

---

## What it costs

Honest numbers, measured with `hydra-gpu-probe.ps1` — CPU over 60 s of
full-screen video on seat B (2026-10-08, Windows 11 25H2):

| | |
|---|---|
| seat B video, `hydra7-avc.ps1` (H.264 on Quick Sync, mstsc) | ~26–30 CPU-seconds a minute — **under half a processor**, 60 fps cap |
| seat B video, `hydra7.ps1` (RemoteFX, FreeRDP) | ~131 CPU-seconds a minute — about **two processors**, 30 fps cap |
| seat B documents, browsing, slides | fine |
| console seat | unaffected |

Seat B is **not** composed in software, as this README used to say: measured,
its desktop is drawn on the iGPU. The cost was RemoteFX — every frame encoded
and decoded on the CPU. Three registry policies, TCP only, and mstsc in place of
FreeRDP move that onto the GPU, and the seat's browsers need AV1 blocked because
this iGPU cannot decode it. [`VIDEO-H264.md`](VIDEO-H264.md) has the
measurements, the recipe and the traps.

---

## Requirements

- Windows 11 (developed on 24H2, build 26100; the H.264 path measured on 25H2,
  build 26200)
- A second monitor, keyboard and mouse
- A second local user account for the seat
- Test-signing enabled if you build the optional display driver
- **Smart App Control OFF** — it blocks unsigned builds, and turning it off is
  permanent

Third-party components — Interception, FreeRDP for the `hydra7.ps1` fallback,
and optionally the Virtual Display Driver — are **not** included. See
`THIRD-PARTY.md` for what to fetch and from where. The H.264 client, mstsc, is
part of Windows.

**Check your Windows licensing.** `tools/termsrv-guard` patches `termsrv.dll` to allow concurrent sessions on a
client SKU. Hydra was built for a single-user machine where both seats are the
same person. Your situation may differ.

---

## Getting started

For work toward more than two directly connected local seats, see
[`LOCAL-MULTISEAT.md`](LOCAL-MULTISEAT.md). It documents the four-user template,
offline checks, input-isolation fixes and the experimental seat-scoped launcher.

**Start with [`INSTALL.md`](INSTALL.md)** — a start-to-finish guide written for
someone who has never seen this project. Prerequisites, the seat account, the
third-party components, building, finding your own hardware IDs and audio
endpoint, first run, and how to back it all up.

Then:

1. [`MODES.md`](MODES.md) — the seven modes, which to use, and every failure
   with its cause. The most useful document here.
2. [`THIRD-PARTY.md`](THIRD-PARTY.md) — what Hydra depends on and under what
   terms.
3. [`REBUILD.md`](REBUILD.md) — rebuilding the machine after a Windows reset.
4. [`tools/termsrv-guard`](tools/termsrv-guard/README.md) — keeps the second
   session working across Windows updates.
5. [`VIDEO-H264.md`](VIDEO-H264.md) — the one-time setup that makes video on
   seat B cheap.
6. [`STATE.md`](STATE.md) — where things stand right now.
7. [`ROADMAP.md`](ROADMAP.md) — what's being worked on next.

```powershell
.\hydra7-avc.ps1          # up -- H.264; see VIDEO-H264.md for the one-time setup
.\hydra7-avc.ps1 -Stop    # down
.\hydra7.ps1              # the RemoteFX original, still works
```

Panic, any time: type `Stop-Service Hydra` and press Enter — works blind, and
releases the captured input in about two seconds.

---

## The modes

Seven approaches to getting seat B's pixels onto its monitor, kept because each
teaches something and the older ones still work as fallbacks.

**Mode 7** is the one to use: the RDP client runs fullscreen on the seat's own
panel and *is* the seat's screen. No capture, no shared memory, no compositing.
Best picture, less than half the CPU of the alternatives. `hydra7-avc.ps1` is
mode 7 with mstsc and H.264 on the iGPU — about a fifth of the CPU of
`hydra7.ps1`'s FreeRDP and RemoteFX for video.

**Mode 6** puts the client on a virtual display and mirrors it to the panel. More
expensive, but it is the shape needed for a third seat.

**Modes 1–3** are earlier designs — mstsc, FreeRDP with Desktop Duplication, and
a headless client. All still work.

**Mode 5** is a working IddCx virtual display driver of our own.

**Mode 4** would have given the seat's session its own display with no client at
all. The driver works. It is closed anyway — see below.

`MODES.md` has the detail.

---

## What is documented here that might be useful elsewhere

The debugging notes are, in places, the most valuable part of this repository.
Several of these cost days.

**`0xD000000D` — UMDF refusing to load a driver before `DriverEntry`.** The
cause was two missing compile defines, `UMDF_VERSION_MAJOR` and
`UMDF_VERSION_MINOR`, which the WDK's MSBuild targets supply automatically and a
hand-driven `cl` line does not. `WDF_BIND_INFO` is built from them and handed to
the framework by `FxDriverEntryUm` *before* `DriverEntry`, so the bind was
malformed. Five days, two defines.

**IddCx required fields that produce a bare `STATUS_INVALID_PARAMETER`.**
`EndPointDiagnostics.pFirmwareVersion` and `pHardwareVersion` must be non-null.
`totalSize` must equal `activeSize` in the signal info — an indirect display has
no blanking interval, and `IddCxMonitorArrival` rejects a mode that claims one.

**`WUDFHost` runs as LOCAL SERVICE.** A driver logging to `C:\Windows\Temp` will
silently write nothing, and the missing log file looks exactly like the driver
never running. That misread cost five days on its own.

**RDP's own indirect display is destroyed with the connection.** Tested
directly: kill the client and `EnumOutputs` returns nothing within seconds. A
disconnected session has no display, so there is no way to capture one.

**A loopback RDP session is drawn on the GPU — and the GPU policy can break it.**
On 25H2 the seat's `dwm` draws on the Iris Plus with no policy set. Setting "Use
hardware graphics adapters for all RDS sessions" moved it onto the 1660 Ti,
whose H.264 encoder then took `RdpIdd.dll` down 14 seconds in (reason 17).

**RDP over UDP on a loopback drops.** mstsc adds a UDP transport; on 127.0.0.2 it
closed the connection every few minutes and reconnected, taking the seat's input
with it each time. `fClientDisableUDP = 1` ended it. `VIDEO-H264.md` has both.

**Windows recycles `oemNN.inf` numbers.** A stale class instance key can end up
referencing a number that now belongs to a different driver, and the resulting
failures look like driver bugs.

`INCIDENT-2026-08-12.md` documents a boot loop that cost an OS reinstall, and
what to do differently. `safety-gate.ps1` was written the day after.

---

## Why mode 4 is closed

Mode 4 would give seat B's session a display directly, removing the RDP client
from the pixel path entirely. **The driver works** — a remote-session IddCx
driver that loads, initialises with
`IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER`, and presents a monitor.

What remains is `GetInputHandles`, documented as returning a handle to an
I8042prt keyboard driver and a Mouclass driver. A protocol provider is expected
to ship its own virtual input miniports.

**Two more kernel drivers, to remove one user-mode process that modes 3, 6 and 7
already handle.** Not a trade worth making. `MODE4-PROVIDER.md` has the full
state if anyone wants to finish it.

---

## Licence

**AGPL-3.0.** See `LICENSE`.

Use it for anything, including commercially, in a company or a school, with no
obligation to publish. If you distribute a modified version or offer it as a
network service, publish your source under AGPL-3.0 too.

Freely available. Not closable.

---

## Warning

Hydra manipulates kernel input filters, installs display drivers, and patches
`termsrv.dll`. **It has broken the machine it was developed
on, twice.**

Do not develop against a machine you cannot afford to lose. Use
`safety-gate.ps1`, which requires a tested bootable recovery stick before any
driver operation, and `hydra-backup.ps1`, which puts everything needed for a
rebuild somewhere a Windows reset cannot reach.

No warranty. Read `MODES.md` first.
