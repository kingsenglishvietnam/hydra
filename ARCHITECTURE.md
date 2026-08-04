# Hydra — unified local multiseat (v4 design)

*(codename placeholder; multi-headed, one body — rename at will)*

This is the design that folds the v1–v3 input router, the cursor clamp, and the
supervisor into **one product** and replaces the RDP display transport with a
local virtual-display + mirror path. It is the honest version of "a lightweight
ASTER": everything ASTER does that is buildable without patching undocumented
win32k internals, and nothing that isn't.

Read the two walls first. They are the whole reason the design looks the way it
does, and they are where every "why didn't you just…" question terminates.

---

## Implementation status (this build)

The scaffold this document described has been completed. What changed:

- **`iddseat` — all three fill-ins done.** (a) Per-seat EDID is generated from the
  configured mode and the three IddCx mode-enumeration callbacks are implemented;
  (b) the shared-surface **producer** is implemented (named metadata section +
  named keyed-mutex texture, published under `Global\HydraSeat_<seat>_…`);
  (c) the D3D device is built on the LUID IddCx reports and that LUID is published
  for the presenter to match. Seat identity/mode/EDID reach the adapter-scoped
  callback via a WDF adapter context, so seats B and C don't collide.
- **`mirror` — both fill-ins done.** Opens the named metadata + surface (consumer
  half of the IPC), builds its device on the published LUID (zero-copy), and
  presents with a single keyed-mutex key.
- **`hydrad`/`hydractl` — built.** Config-driven `SwDeviceCreate` per seat,
  cross-session launch + supervision (absorbing `respawn`'s exit-code + backoff
  contract), and a control pipe (`status`/`reload`/`restart`/`learn`/`stop`).
- **New shared pieces**, all header-only and unit-tested natively: `hydra_edid.h`
  (EDID build/parse — also validated with `edid-decode`), `hydra_ipc.h` (the
  producer/consumer contract), `hydra_config.h` (`seats.toml` parser +
  `hydra_build_router_args`), `hydra_devprops.h` (shared device-property keys).

See **`BUILD.md`** for the build/test-sign/install order and a precise
compile-verified-vs-scaffold breakdown. Risk #1 below is still the experimental
part; Risk #2 is now resolved (see its entry).

---

## The two walls

### Wall 1 — you cannot mint a concurrent interactive session without Terminal Services

Windows client is architecturally single-interactive-user. The *only* mechanism
that produces a second logged-in interactive session is Terminal Services; that
is exactly what RDP-Wrapper patches `termsrv.dll` to permit. There is no
supported or unsupported alternative that yields an independent session with its
own desktop, cursor, focus and input queue.

Consequences:

- **"Bypass RDP altogether" is only half-true.** You can delete the RDP
  *protocol* (mstsc encode → loopback → decode). You cannot delete the RDP
  *session*: the extra seat is, and remains, a TS session. Hydra still depends on
  RDP-Wrapper (or equivalent) to *create* the session; it just stops using RDP to
  *display* it.
- **`CreateDesktop` is not a shortcut.** A second desktop object lives in the same
  session and does have its own input queue and cursor — but `SwitchDesktop` is
  winstation-global: only one desktop is on the physical display at any instant.
  You cannot show two desktops on two monitors at once. This is the same
  mechanism that blanks everything when the UAC secure desktop appears. Dead end
  for simultaneous seats.

### Wall 2 — a second physical monitor cannot be driven as an independent seat within one session, and no user-mode code can scan out to a physical connector

- **Single-session multiseat (true ASTER)** requires multiplexing one desktop's
  single input queue into N logical cursors/focus and binding a shell per seat per
  monitor. win32k does not expose this. ASTER achieves it by hooking/patching
  win32k internals that Microsoft does not document and that shift with feature
  updates. It is a multi-year, perpetually-maintained reverse-engineering effort.
  There is no robust open equivalent, and there will not be one written in a
  weekend. Hydra does **not** attempt this.
- **Direct scanout is vendor-only.** Putting pixels on a physical connector is the
  job of the WDDM display miniport that owns that GPU — NVIDIA's driver for the
  1660 Ti. Writing one needs hardware programming documentation that is not
  public. An IddCx driver never touches a connector; it hands you a *surface* and
  someone in user mode presents it. So the physical monitor for an extra seat is
  always driven by **the console session compositing a mirror**, never by the
  virtual display "reaching" the panel directly.

Everything below is built to stay on the buildable side of both walls.

---

## What "the kernel display driver" actually is

The display driver you want is an **Indirect Display Driver (IddCx)**. Important:
it is a **UMDF (user-mode) driver**, hosted in `Wudfhost.exe`. It is not
kernel-mode. This is a feature:

- it cannot bugcheck the machine;
- it needs no GPU hardware docs;
- it is built with the WDK against the documented IddCx class extension.

A true *kernel* display driver (WDDM display miniport) is the vendor artifact
described in Wall 2 and is out of scope permanently. If you ever see this project
described as shipping a "kernel display driver," that description is wrong — it
ships a UMDF indirect display driver.

What IddCx gives you: one or more **virtual monitors**. The OS renders a desktop
into them exactly as it would a real panel, and your driver receives each frame
as a Direct3D texture. What you do with that texture is the entire trick.

---

## The v4 topology

Per extra seat, the full loop, RDP-protocol-free:

```
  seat B keyboard/mouse (physical, console session)
        │
        │  captured + blocked by Interception
        ▼
  ┌───────────────┐        loopback TCP (9-byte WireEvent, v3 wire)
  │  input router │ ───────────────────────────────────────────────┐
  │ (console/A)   │                                                 │
  └───────────────┘                                                 ▼
                                                          ┌──────────────────┐
                                                          │  seatB_agent     │
                                                          │  (session B)     │
                                                          │  SendInput()     │
                                                          └──────────────────┘
                                                                    │
                                     session B input queue ◄────────┘
                                                                    │
                                     session B renders its desktop  │
                                     (including its own cursor)      ▼
                                                          ┌──────────────────┐
                                                          │  iddseat  (IDD)  │
                                                          │  virtual monitor │
                                                          │  → D3D texture   │
                                                          └──────────────────┘
                                                                    │
                                        shared surface (NT handle,  │
                                        keyed mutex, cross-session)  │
                                                                    ▼
  physical monitor 2  ◄────── fullscreen borderless present ── ┌──────────────────┐
  (owned by console/A)                                         │  mirror          │
                                                               │  (console/A)     │
                                                               │  DXGI present    │
                                                               └──────────────────┘

  console cursor confined to monitor 1 by the clamp (clip), so seat A never
  intrudes on the surface showing seat B.
```

Why this closes cleanly:

- **Input isolation is already solved** by v1–v3 and is unchanged. The agent
  `SendInput`s into session B directly, so the seat never depends on a viewer
  window holding focus — which is what makes replacing mstsc with a dumb
  fullscreen surface *safe*. (Under mstsc it was not safe: the RDP window had to
  own focus to receive input. That entire coupling is gone.)
- **Session B's cursor is part of session B's framebuffer**, so it appears in the
  mirrored image automatically. No second cursor to composite.
- **The mirror owns the physical monitor as a locked fullscreen surface** — no
  window chrome, no focus semantics, no alt-tab, nothing for `clip_console` to
  fight. The clamp's remaining job shrinks to keeping the *console* cursor off
  monitor 2.

### What each component becomes

| v3 today | v4 role |
|---|---|
| `seat_router.c` | **module** inside the host: Interception capture + wire forward. Already multi-seat. Near-unchanged. |
| `seatB_agent.c` | **unchanged.** Runs in each session, replays input. It never knew about display and still doesn't. |
| `clip_console.c` | **module**: confine console cursor to seat A's monitor(s). Unchanged mechanics; smaller responsibility. |
| `respawn.c` | **absorbed** into the host's supervision (same backoff + exit-code contract, driven by config instead of a command line). |
| — | `iddseat` — **new.** UMDF IddCx driver, one virtual monitor per extra seat. |
| — | `mirror` — **new.** Per-seat DXGI presenter: virtual surface → physical monitor. The RDP-transport replacement. |
| — | `hydrad` / `hydractl` — **new.** Single control plane: one config, discovery, lifecycle, logging. |

---

## The control plane (`hydrad` + `hydractl`)

The point of "one product" is that a human configures seats *once*, declaratively,
and a single service makes reality match. No five console windows.

### Config (single source of truth)

`seats.toml` (or JSON — pick one; TOML shown for readability):

```toml
# console/seat A keeps whatever monitors aren't claimed below.
[hostA]
confine_monitor = "\\\\.\\DISPLAY1"   # clip target for the console cursor

[[seat]]
name      = "B"
kbd       = 2                          # Interception device numbers (see --learn)
mouse     = 12
port      = 56789
monitor   = "\\\\.\\DISPLAY2"          # physical panel this seat is shown on
session   = "auto"                     # TS session to bind (see integration risks)
edid      = "1920x1080@60"             # virtual monitor mode the IDD advertises

[[seat]]
name      = "C"
kbd       = 3
mouse     = 13
port      = 56790
monitor   = "\\\\.\\DISPLAY3"
session   = "auto"
edid      = "2560x1440@60"
```

Monitors are addressed by **device name**, not index — same decision as
`clip_console`, and for the same reason (indices renumber on replug/rescale).

### `hydrad` responsibilities (console session)

1. **Ensure the virtual displays exist.** Instantiate one `iddseat` software
   device per extra seat (software-device install, e.g. via the SWDevice API or a
   bundled `devcon`/`nefcon`), each advertising that seat's configured mode.
2. **Ensure sessions exist and are logged in.** Still TS. Hydra can trigger the
   session (RDP-Wrapper must be present) but *does not* pretend to replace it.
   This step is the softest — see risks.
3. **Bind each virtual monitor to its seat's session** so that session renders
   into it. This is the fiddliest step in the whole design (integration risk #1).
4. **Run the input router module** (Interception) across all configured seats.
5. **Run the clamp module** for the console.
6. **Supervise the per-seat `mirror` and, inside each session, `seatB_agent`** with
   the v3 backoff + exit-code contract. Cross-session process launch needs the
   session token (`WTSQueryUserToken` + `CreateProcessAsUser`) — the host runs as a
   service (LocalSystem) precisely so it can do this.
7. **One log stream, one health view.** `hydractl status` shows every seat: session
   up?, IDD present?, mirror presenting?, agent connected?, last heartbeat, drop
   counters.

`hydractl` is a thin CLI over the service: `status`, `reload`, `learn` (the
Interception device enumerator), `seat B restart`, etc.

### Supervision model (unchanged philosophy)

The v3 layering holds: each module heals its own faults and its hang-watchdog
converts an undetectable wedge into a detectable death; the host restarts on
faults with capped backoff and honors the exit-code contract (0 = deliberate,
2 = config error → do not restart; else → restart). The host is now the single
supervisor for all of it, driven by `seats.toml` instead of per-process argv.

---

## The mirror (RDP-transport replacement)

Standard low-latency present loop; the honest core:

1. Create a D3D11 device on the **same adapter LUID** the IDD's render device
   used. Same-GPU is the zero-copy fast path. Cross-GPU forces an inter-adapter
   copy and gives up the latency win — so bind the virtual monitor's render
   adapter to the physical GPU deliberately.
2. Receive the seat's **shared surface handle** from `iddseat` over the host's IPC
   (NT handle via `IDXGIResource1::CreateSharedHandle`, duplicated into the mirror
   process; keyed mutex for sync).
3. Create a flip-model swapchain on a **fullscreen borderless window placed on the
   seat's physical monitor** (reuse `clip_console`'s monitor-by-device-name
   enumeration to position it).
4. Loop: acquire keyed mutex → copy/draw the shared texture into the backbuffer →
   `Present(1, 0)` → release mutex. Vsync-paced; no encode, no decode, no network.

Latency vs. mstsc-over-loopback: modestly better for productivity (you drop the
codec, not a network hop that was already localhost). The larger, more certain win
is architectural — no viewer-window class of bugs, no compression artifacts,
deterministic present.

---

## Build order (do not build it all at once)

Each step is independently testable. Do not proceed until the prior one is real.

1. **`iddseat` shows one virtual monitor.** Fork the WDK **IndirectDisplay** sample,
   get a virtual 1080p monitor to appear in Display Settings on the *console*
   first (ignore sessions/mirror). Prove the driver installs, loads, and the OS
   extends onto it. This is the "does the kernel-adjacent bit work at all" gate.
2. **`mirror` presents that surface to a physical monitor**, still all in the
   console session. Extend the console desktop onto the virtual monitor, mirror it
   fullscreen onto a second physical panel. Now you have a working
   virtual→physical present path with zero session complexity. This de-risks the
   two new subsystems before the hard part.
3. **Cross the session boundary.** Bind the virtual monitor to a *TS session's*
   desktop and share its surface back to the console mirror. **This is where the
   real risk lives** (integration risk #1). Expect iteration.
4. **Fold in the v3 router + clamp + agent** under the host, config-driven.
5. **`hydrad`/`hydractl`**: discovery, lifecycle, one config, one status view.

If step 3 proves intractable in your environment, you still have a coherent
fallback: keep mstsc for display but ship everything else unified (steps 4–5).
That is strictly better than today even without the mirror.

---

## Known limits / integration risks (read before committing weeks to this)

- **Risk #1 — binding a virtual monitor to a TS session's desktop.** IddCx devices
  are system devices; getting a *specific TS session* to render into a specific
  virtual monitor (rather than the console) is the genuinely fiddly, under-
  documented part. Options range from per-session driver instantiation to display-
  affinity finesse; none is a clean documented one-liner. Budget real
  experimentation here. This is the single most likely thing to sink the display
  half.
- **Risk #2 — cross-session shared surfaces. (Resolved.)** Instead of duplicating
  a D3D texture handle across the session boundary, the producer creates the
  metadata section and the keyed-mutex texture as **named objects in the `Global\`
  namespace** and the presenter opens them by name (`OpenSharedResourceByName`) —
  no cross-session `DuplicateHandle`, no handle-lifetime dance. Contention is
  bounded to a **single** keyed-mutex key ("latest frame wins") so a slow
  presenter can never stall the driver's swap-chain thread. This is the one risk
  the original design flagged that is now closed in code.
- **Session creation is still TS.** Hydra triggers it; it does not replace it.
  RDP-Wrapper's Windows-client concurrent-session licensing caveat is inherited
  unchanged — this is your call, as it was in v1–v3.
- **Driver signing.** IddCx/UMDF still loads through the kernel graphics stack and
  requires a signed driver package. Development means **test-signing** (enable test
  mode, sign with a self-signed cert) — a reboot and a desktop watermark. Shipping
  to another machine means either that machine in test mode or an EV-signed +
  attestation-signed package. There is no unsigned path.
- **You are rebuilding a working transport.** Restated because it matters: the
  capability (concurrent sessions) does not change. The wins are polish, latency,
  and deleting the mstsc-window bug class — not multiseat-that-was-impossible-
  before. If mstsc-over-loopback is acceptable, steps 4–5 alone (unify, keep
  mstsc) capture most of the "one product" value for a fraction of the effort.
- **Direct scanout remains impossible** from user mode (Wall 2). The console always
  composites the mirror. If you ever want true independent GPU scanout per seat,
  that is a hardware/vendor problem, not a software one.
- **Not twitch-gaming.** The mirror removes the codec but not the fundamental
  "render in session B, copy, present in session A" round trip. Fine for office
  work and teaching; not a substitute for a native GPU output under a competitive
  shooter.

---

## File map

```
hydra/
  ARCHITECTURE.md        this document
  BUILD.md               build / test-sign / install guide
  README.md              original v3 input-stack notes
  build.ps1              builds all SDK-buildable components into dist\
  seats.toml             sample configuration
  common/
    hydra_edid.h         EDID 1.4 build/parse (header-only; unit-tested)
    hydra_ipc.h          producer/consumer shared-surface contract (header-only)
    hydra_devprops.h     shared seat-name / seat-mode device-property keys
  iddseat/
    iddseat.h            IddCx driver: types, callbacks, contexts
    iddseat.cpp          driver entry, adapter/monitor lifecycle, surface producer
    iddseat.inf          install manifest (multi-instance; TODOs resolved)
  mirror/
    mirror.cpp           DXGI present loop: shared virtual surface → physical panel
  hydrad/
    hydra_config.h       seats.toml parser + router-arg builder (header-only; tested)
    hydrad.cpp           control plane: SwDevice per seat, cross-session supervision
  hydractl/
    hydractl.cpp         thin control-pipe CLI (status/reload/restart/learn/stop)
  input/                 the verified v3 stack (compile-checked with MinGW)
    seat_router.c        input router      (console session)
    seatB_agent.c        replay agent      (per seat session)
    clip_console.c       cursor clamp      (console session; now takes device names)
    respawn.c            supervisor        (contract absorbed by hydrad)
  tests/
    test_edid.c          native EDID round-trip test  (passes)
    test_config.cpp      native config + router-arg test (passes)
```

`iddseat`, `mirror`, `hydrad`, and `hydractl` are **WDK/MSVC build artifacts**
(IddCx + D3D11/DXGI + SwDevice/WTS). The driver and D3D pieces cannot be
cross-compiled with the MinGW toolchain that built the input stack; their
pure logic (EDID, IPC names, config, router args, mode parsing) is extracted into
the header-only pieces above and unit-tested natively. See `BUILD.md` for the
exact compile-verified-vs-scaffold breakdown. Treat first bring-up as bring-up.

