# VIDEO-H264.md — seat B video on the iGPU

Measured from 2026-10-08 on the Surface Book 3, Windows 11 Pro 25H2 (build 26200),
with `hydra-gpu-probe.ps1`: CPU-seconds per process over a 60 s window, the same
clip full-screen on seat B.

**Short version:** seat B was never composed in software. The cost was the codec.
Three registry policies, TCP only, mstsc as the client and AV1 blocked in the
seat's browsers take a minute of full-screen video from about two processors to
under half of one.

---

## What the old explanation got wrong

README and INSTALL said a loopback RDP session composes its desktop in software
(WARP), and that a client SKU cannot do better. Measured, seat B's `dwm.exe`
draws on the Iris Plus: every GPU Engine counter for it sits on the Intel
adapter's LUID, none on Basic Render, and it spends about zero CPU-seconds. That
holds with no policy set at all.

The cost was RemoteFX: every frame encoded by TermService and decoded by the
client, both on the CPU. Event 162 shows why it never left the CPU — FreeRDP as
`hydra7.ps1` launches it (`/gfx:rfxc`) advertises `AVC available: 0`, so the
server cannot choose H.264.

The old 25–28 fps figures also sit just under RDP's default 30 fps cap
(KB 2885213).

## The numbers

| CPU-seconds per 60 s of video | RemoteFX, sdl-freerdp | H.264 in software | H.264 on Quick Sync |
|---|---|---|---|
| TermService (encode) | 49.7 | 88.4 | 11.9 |
| WUDFHost (RdpIdd) | <1 | 17.7 | <4 |
| client | 80.7 | 16.4 | 10.3 |
| console dwm | <1 | 5.9 | 3.9 |
| **pipeline** | **~131** | **~128** | **~26–30** |

RemoteFX ran under the 30 fps cap, with ten windowed seconds in the window; both
H.264 runs under a 60 fps cap, full-screen throughout. The software-H.264 run had
seat B on the 1660 Ti (trap 1). With AV1 blocked (trap 3), Chrome on seat B fell
from about 45 CPU-seconds to about 11, decoding on the Iris Plus.

---

## The recipe

| Where | Value | Why |
|---|---|---|
| `HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services` | `AVC444ModePreferred` = 1 | H.264 4:4:4 |
| | `AVCHardwareEncodePreferred` = 1 | encoded on Quick Sync |
| | `bEnumerateHWBeforeSW` **absent** | trap 1 |
| `...\Terminal Services\Client` | `fClientDisableUDP` = 1 | trap 2 |
| `HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations` | `DWMFRAMEINTERVAL` = 15 | 60 fps instead of 30 |
| seat B's browsers | AV1 blocked | trap 3 |
| client | mstsc, via `hydra7-avc.ps1` | decodes H.264 on the GPU (FreeRDP: on the CPU, see below) |

`.\hydra-gpu-probe.ps1 -Apply` sets the registry part after a snapshot under
`safety\`; `-Revert` restores the snapshot. Reboot after either.

---

## Traps, each found the hard way

**1. `bEnumerateHWBeforeSW` makes it worse.** "Use hardware graphics adapters for
all RDS sessions" moved seat B off the Iris Plus and onto the 1660 Ti. With
hardware encoding on, RDP then chose the NVIDIA H.264 Encoder MFT, and `RdpIdd.dll`
crashed about 14 seconds in: disconnect reason 17, DriverFrameworks-UserMode
10121, and `nvcontainer.exe` a second later. Without the policy the session stays
on the Iris Plus and event 170 names Intel's Quick Sync encoder. `hydra7-avc.ps1`
refuses to start while the policy is set.

**2. RDP over UDP on the loopback drops.** mstsc opens a UDP transport beside TCP
(event 1102). On 127.0.0.2 it closed the connection every few minutes — client
1026 `Reason= 2308` (socket closed), server event 40 code 0x80070040, 1105 in the
same second — then auto-reconnected within 1–10 seconds (event 40 reason 5, event
25). Each gap looked like buffering, and agent:B logged `SendInput ... (err 5)`
until the session came back, so the student's keyboard and mouse died with every
drop. `fClientDisableUDP = 1` ended it. FreeRDP only ever used TCP.

**3. Ice Lake has no AV1 decoder.** YouTube served `av01`, which Chrome decoded on
the CPU. Block AV1 in every browser on seat B:

- **Firefox:** `about:config` → `media.av1.enabled` → `false`.
- **Chrome, Brave, Edge, Opera:** enhanced-h264ify from the Chrome Web Store,
  with only AV1 ticked, so YouTube serves VP9, which the Iris Plus decodes in
  hardware. Not the old h264ify: it blocks VP9 and leaves AV1.
- **Any browser, signed in:** YouTube → Settings → Playback and performance → AV1
  settings → *Prefer AV1 for SD*.

Stats for nerds should then say `vp09`.

**4. mstsc ignores `selectedmonitors` here.** It opened windowed on the laptop.
`hydra7-avc.ps1` places the window with the window APIs and picks "Full screen"
from mstsc's own window menu (`WM_SYSCOMMAND`), then checks the window covers
exactly the panel.

**5. Neither client draws the student's cursor.** An RDP client never learns
where the seat's pointer is -- the server sends a position only when an
application warps the cursor -- and it shows the pointer image as its own local
cursor, which belongs to the console mouse. Pointer trails (a software cursor)
don't change that inside an RDP session, and FreeRDP behaves exactly like mstsc.
An overlay running *inside* seat B worked, but lost to Task View and to virtual
desktop switches. The fix: agent:B publishes the seat's cursor
(`Global\HydraSeat_B_cursor`), and `cursor_overlay_seat.exe` draws it over the
seat's panel from the console session, where seat B's shell can't touch it. See
"The seat's cursor" below.

**6. Audio pins accumulate.** Windows keeps one per-app entry per change and never
prunes them. `-Save` captured a stale Realtek entry beside the monitor's, `-Apply`
restored both, and seat B played nowhere. Keep only the entry naming `intcdaud`;
`hydra7-avc.ps1` warns when there are others.

---

## Running it

```powershell
.\hydra7-avc.ps1          # up
.\hydra7-avc.ps1 -Stop    # down
```

One-time audio pin, with seat B playing sound:

1. `Start-Process ms-settings:apps-volume` → mstsc ("Remote Desktop Connection")
   → Output device → the 2770 (Intel Display Audio).
2. `.\audio-pin.ps1 -Save -App 'mstsc.exe' -StateFile .\audio-pin-mstsc.json`
3. The file should hold one entry, naming `intcdaud`.

## The seat's cursor

Build once, from the Visual Studio x64 prompt with the Hydra service stopped
(`.\hydra7-avc.ps1 -Stop`). `.\build.ps1` builds the agent too, along with
everything else:

```powershell
cl /nologo /O2 /W3 /MT /Iinput input\seatB_agent.c /Fo:dist\ /Fe:dist\seatB_agent.exe `
    /link ws2_32.lib user32.lib winmm.lib advapi32.lib
cl /nologo /O2 /EHsc /W3 overlay\cursor_overlay.cpp /Fo:dist\ /Fe:dist\cursor_overlay_seat.exe
```

`hydra7-avc.ps1` then starts the overlay with the seat:
`cursor_overlay_seat.exe 8 --seat B --at X,Y,W,H`, the panel's rectangle in
physical pixels. The agent publishes about 120 times a second. The overlay is a
pointer-sized window that follows the seat's pointer, drawn with per-pixel alpha
(no colour-key fringe), hidden when the agent goes quiet for a second, and it
follows the console's virtual desktops.

It draws the seat's **own** pointer: whatever scheme, colour and size seat B's
mouse settings choose, re-checked once a second so a settings change shows up.
For standard pointers the agent loads seat B's pointer *file* from its scheme
(`HKU\<sid>\Control Panel\Cursors`) at 256 px. Windows writes those at the
chosen size and colour -- a maximum-size custom-colour arrow is a single 256 px
`arrow_eoa.cur` -- while the live cursor handle in the RDP session reads back at
32 px no matter what. So the image is sharp, and `-OverlayScale` multiplies the
32 px on-screen size: x8 draws the 256 px original 1:1, anything smaller is a
clean shrink. Application-drawn pointers fall back to the live 32 px image. An
"inverted" pointer flips whatever is underneath it, which a picture can't do,
so it comes out black.

## Checking it

```powershell
.\hydra-gpu-probe.ps1     # with the same clip full-screen on seat B
```

What to look for:

- **Composition:** the verdict says Iris Plus.
- **Encoder:** event 170 names Intel® Quick Sync Video H.264 Encoder MFT.
- **Decode:** the browser's `videodecode` engine is on the Iris Plus.
- **Transport:** the RDPClient section shows 0 UDP attempts and 0 drops.

---

## Also measured: FreeRDP with H.264

The FreeRDP 3.30.0 build in `dist` (MSYS2) does decode H.264. `/gfx:avc444`
negotiated graphics protocol 10.7 with `AVC available: 1`, profile 2048, Quick
Sync on the server. `/gfx:avc420` instead fell back to protocol 8.1 *without*
H.264, and `/gfx:h264` is the crash STATE.md records. Decoding is FFmpeg on the
CPU: about 55 CPU-seconds a minute in the client, about 70 for the pipeline,
against mstsc's 26–30. It doesn't draw the student's cursor either (trap 5), so
mstsc stays.

## Also measured: the VM route is open

With Hyper-V on, `Get-VMHostPartitionableGpu` lists **both** the Iris Plus and the
1660 Ti. FUTURE.md's mode 10 assumed passthrough; partitioning (GPU-P) works
here. Not pursued — this document removed the reason.
