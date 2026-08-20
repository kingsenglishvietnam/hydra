# Questions worth asking now — things testable on this machine

Mode 4 is closed. These are questions about **what you already run**, answerable
on the Surface Book 3 as it stands, with no new hardware and no new drivers.

Ordered by how much they would change daily use. All postable without
identifying the project.

---

## 1. RDP video performance — the one that could affect a lesson

**Where:** FreeRDP GitHub Discussions. They know their own codec paths better
than anyone.

> ### Best FreeRDP client settings for full-screen video over a loopback connection?
>
> I run `sdl-freerdp` against `127.0.0.2` on the same machine — no network at
> all, so bandwidth is free and latency is nil. The bottleneck is entirely
> encode/decode CPU and the fact that the server session does software
> rendering.
>
> Playing full-screen video in the session is choppy. What I have:
>
> ```
> /v:127.0.0.2 /u:<user> /d: /cert:ignore /sound -suppress-output
> /scale:140 +auto-reconnect /f /monitors:N
> ```
>
> 1. For a **loopback** connection, which codec path is cheapest end to end?
>    RemoteFX, progressive, or plain bitmap updates? The usual advice optimises
>    for bandwidth, which is the wrong axis here.
> 2. Does `/network:lan` change anything meaningful when there is no network, or
>    is it purely a bandwidth heuristic?
> 3. Is there a way to get **hardware decode** in the client while the server
>    session renders in software, or does that gain nothing?
> 4. This build is `WITH_VAAPI_H264_ENCODING=ON` and `/gfx:h264` crashes it on
>    Windows. Is that expected for a Windows build, and is there a supported
>    H.264 path on Windows clients?
> 5. Would `/gdi:hw` help or hurt for a client whose output is being captured by
>    Desktop Duplication rather than displayed directly?

Include your exact command line. It is a well-specified question with an unusual
constraint — no network — and that makes it interesting to answer.

---

## 2. Whether Windows can be told to render a session's desktop in hardware

**Where:** Microsoft Q&A, `windows-server-rds`. Long shot, high payoff.

> ### Can an RDS session use GPU acceleration for its own desktop composition on a client SKU?
>
> A remote session's DWM composes in software by default. On Server SKUs there
> is RemoteFX vGPU / GPU-accelerated RDS. On client SKUs, is there any supported
> way to have a session's desktop composed with hardware acceleration?
>
> The machine has both an Intel iGPU and a discrete NVIDIA GPU. The session is
> local — loopback RDP on a single-user machine, not a real remote connection.
>
> 1. Is `RemoteFX` / hardware-accelerated composition gated by SKU, by policy,
>    or by the absence of the RDS role?
> 2. Do the `Terminal Services` group policies for graphics
>    (`Use hardware graphics adapters for all Remote Desktop Services sessions`)
>    do anything on a client SKU?
> 3. Does session GPU assignment depend on the driver's WDDM version or on the
>    adapter being enumerated inside the session?

If any of that is reachable, it would fix the one real limitation of every mode
Hydra has — and it costs nothing to ask.

---

## 3. Mode 6 scaling — how many seats before it falls over

**Where:** the VDD project's Discussions, and r/VirtualDisplay if it exists.

> ### Practical limits on multiple simultaneous IddCx virtual displays?
>
> Running one virtual display via VDD, considering two or three. Each would host
> a full-screen client for a separate local session.
>
> 1. What is the practical ceiling on simultaneous virtual displays before
>    composition costs dominate? Does it depend on the GPU or on DWM?
> 2. Does `<count>` in `vdd_settings.xml` create genuinely independent displays,
>    or one adapter with several outputs — and does that matter for Desktop
>    Duplication running against each?
> 3. Anyone running two or more VDD displays with something full-screen on each?
>    Interested in real CPU and memory numbers rather than theory.

This directly informs whether a third seat is realistic before you promise one to
anyone.

---

## 4. Desktop Duplication under load — the capture path's ceiling

**Where:** OSR NTDEV, or Stack Overflow with the `directx` and `desktop-duplication`
tags.

> ### DXGI Desktop Duplication: expected throughput with several concurrent duplications on one adapter?
>
> Duplicating one 1920x1080 output at present, measuring ~25–120 fps depending
> on activity. Considering two or three concurrent duplications, each in a
> different session, on the same adapter.
>
> 1. Does `IDXGIOutputDuplication` serialise per adapter, per output, or not at
>    all?
> 2. Is there a known ceiling on concurrent duplications before
>    `AcquireNextFrame` latency climbs?
> 3. For a duplication whose frames are memcpy'd to a shared section for another
>    process, is there a faster supported path than
>    `Map`/`memcpy`/`Unmap` — a shared texture that survives a session boundary?
>
> Cross-session shared D3D11 textures appear not to work
> (`OpenSharedResourceByName` returns `0x80070057` across sessions), which is
> why the copy is on the CPU. Is that expected, and is there a supported
> alternative?

That last one is worth asking on its own. If cross-session texture sharing has a
supported form, modes 1, 2, 3 and 6 all get faster and the CPU copy disappears.

---

## 5. Input injection — is `SendInput` the right tool

**Where:** OSR NTDEV.

> ### Injecting input into another session: SendInput from a session-resident helper, or something better?
>
> I inject keyboard and mouse into a second local session using a helper process
> running inside it, calling `SendInput`. It works, but:
>
> 1. `SendInput` fails with `ERROR_ACCESS_DENIED` on secure desktops, so the
>    session cannot be unlocked from outside. Is there a supported way to inject
>    at a lock screen, or is that firmly by design?
> 2. The helper has to re-attach with `SetThreadDesktop` whenever the input
>    desktop changes. Is there a way to be notified of that rather than
>    discovering it via a failed injection?
> 3. Is there a lower-level supported injection path — something that behaves
>    more like a real HID device to the session — short of writing a virtual HID
>    driver?

Question 1 is worth asking regardless. Seat B cannot unlock itself, which has
been an open limitation since May.

---

## 6. Two small ones, cheap to ask

> ### Does per-application audio output routing survive across sessions?
>
> A per-app output device assignment
> (`HKCU\...\Audio\PolicyConfig\PropertyStore`) is per-user and resets often. Is
> there a machine-wide or more durable form? And is that registry location
> documented anywhere, or is it purely observed behaviour?

> ### Reliable way to keep a window on all virtual desktops from a script?
>
> Using the `VirtualDesktop` PowerShell module's `Pin-Window`. Is there a
> documented COM interface for this that will not break between Windows
> releases? `IVirtualDesktopManager` seems to cover moving but not pinning.

Both affect daily reliability rather than capability.

---

## How to ask these

**They are all ordinary questions.** No architecture, no repo, no classroom —
just a person running loopback RDP on a single-user machine.

**Say the connection is loopback.** It is the unusual constraint and it inverts
most standard advice, which optimises for bandwidth.

**Include real numbers.** "25–120 fps depending on activity" and
"`OpenSharedResourceByName` returns `0x80070057` across sessions" are what make
a question answerable.

**Ask one thing per post.** These are separate audiences — FreeRDP, VDD,
Microsoft, OSR — and a question that spans all four gets answered by none.

---

## Which two to actually ask

If you post two, make them **#1** and **#4's last part**.

**#1** is the only unknown that could affect a lesson, and FreeRDP's maintainers
are responsive.

**#4's cross-session texture sharing question** would, if it has a good answer,
make every capture-based mode faster and remove the CPU copy that is currently
the most expensive thing in the pipeline.

Everything else is worth knowing but not worth waiting on.
