# Questions worth asking, to finish mode 4

The driver is done. What remains is a protocol provider that cannot hold its
session open. These are the questions that would close it, ordered by how likely
they are to get a useful answer.

Each is written so it can be posted **without revealing the project** — no
Hydra, no classroom, no repo. They read as ordinary Windows driver questions,
because that is what they are.

---

## The core question

**Where:** OSR Online, NTDEV list (osronline.com). Second choice: Microsoft Q&A,
tag `windows-hardware-drivers`.

> ### IWRdsProtocolConnection: session disconnects with reason 18 immediately after NotifyCommandProcessCreated
>
> Windows 11 24H2, build 26100. I have a custom RDS protocol provider based on
> the `TestProtocol_Ext` sample, registered as a `WinStations` listener with a
> `LoadableProtocol_Object` COM class. It creates a session successfully — the
> user authenticates (Security 4624, Logon Type 10), the shell process is
> created, and an IddCx remote-session indirect display driver attaches and
> presents a monitor.
>
> Then, roughly 400 ms after `NotifyCommandProcessCreated`, termsrv calls
> `PreDisconnect` with `DisconnectReason = 18`, followed by `DisconnectNotify`
> and `Close`. My `PreDisconnect` only logs and returns `S_OK`, so the provider
> is not initiating this.
>
> ```
> EnableWddmIdd(1)
> AcceptConnection / GetClientData / AuthenticateClientToSession
> NotifySessionId / GetInputHandles / GetHardwareId
> ConnectNotify session=2
> NotifyCommandProcessCreated session=2
> PreDisconnect reason=18          <-- ~400ms later
> DisconnectNotify / Close
> ```
>
> Notably the reason code tracks what is wrong upstream: with a broken IDD I got
> reason 17 at ~1.2 s, with no IDD at all reason 12 at ~33 s, and now with a
> working IDD, reason 18 at ~0.4 s. So 18 appears to be something *after* the
> display is up.
>
> **Questions:**
>
> 1. Is `DisconnectReason` 18 documented anywhere? I cannot find a mapping for
>    the values passed to `IWRdsProtocolConnection::PreDisconnect`.
> 2. Which `IWRdsProtocolConnection` / `IWRdsProtocolConnectionCallback` methods
>    must a provider service *continuously* to keep a session alive? The sample
>    returns `S_OK` from most of them and has no real transport.
> 3. Are the handles returned from `GetInputHandles` expected to remain valid
>    and be actively serviced? If termsrv writes to them and nothing reads, is
>    that a disconnect trigger?
> 4. Is there a heartbeat, keepalive or status method — `GetLastInputTime`,
>    `GetProtocolStatus` — where returning zero or a stub value causes an idle
>    disconnect?

Include your provider log verbatim. It is the strongest part of the question.

---

## The alternative that may skip the problem entirely

**Where:** same places. This one is worth asking *first* if you only ask one.

> ### Can an ordinary RDP session's indirect display be duplicated directly, rather than via the client framebuffer?
>
> `RdpIdd_IndirectDisplay` — Microsoft's in-box remote-session IddCx driver —
> creates a devnode per session under `SWD\REMOTEDISPLAYENUM\`. So an ordinary
> `mstsc` session already has a working indirect display driver attached to it.
>
> Today I get at those pixels by running a client, letting it decode into its
> framebuffer, and capturing that. Is there a supported way to attach to the
> session's IDD output directly — Desktop Duplication against that session's
> display, or an IddCx-side interface — without a client in the path at all?
>
> I am aware DXGI Desktop Duplication is per-session and requires running in the
> target session. Assume I can run a helper process inside it.

If the answer is yes, the provider problem disappears — the session comes from
ordinary RDP, which already works, and only the pixel path changes.

---

## Two smaller ones, worth including as follow-ups

> ### Is there a supported list of what the WDK MSBuild targets inject that a hand-driven cl/link line does not?
>
> I build a UMDF 2 driver outside MSBuild. It failed to load with `0xD000000D`
> at level 0, before `DriverEntry`, until I added `/DUMDF_VERSION_MAJOR=2` and
> `/DUMDF_VERSION_MINOR=<n>` — `WDF_BIND_INFO` is built from those macros. That
> cost several days to find.
>
> Is there a documented list of the other properties `Wdf.props` / `Wdf.targets`
> set? I would rather discover the rest by reading than by failing.

> ### IddCx: which IDDCX_ADAPTER_CAPS and DISPLAYCONFIG_VIDEO_SIGNAL_INFO fields are actually required?
>
> Two `STATUS_INVALID_PARAMETER` failures I eventually solved by diffing against
> the WDK sample:
>
> - `EndPointDiagnostics.pFirmwareVersion` and `pHardwareVersion` must be
>   non-null. The sample comments them "(required)"; the docs do not say so.
> - `totalSize` must equal `activeSize` in the signal info. I had added a
>   blanking interval, which `IddCxMonitorArrival` rejects.
>
> Is there a definitive list of required-vs-optional fields for these structs?
> Both errors surface as a bare `STATUS_INVALID_PARAMETER` with no indication of
> which field is at fault.

---

## How to ask well

**Lead with what works.** "Your driver loads on my machine and mine does not"
got a detailed, useful answer. It signals you have done the work and are not
asking someone to debug from scratch.

**Include the eliminated list.** Nothing wastes more goodwill than a dozen
replies suggesting things you tried on day one. The 0xD000000D question had
nineteen; it is why the reply was substantive.

**Paste logs verbatim, not summaries.** The provider log above is the question.

**Ask a specific question at the end.** "What am I missing?" gets sympathy;
"which methods must be serviced continuously?" gets an answer.

**Nothing here identifies the project.** These are generic Windows driver and
RDS questions. No repo, no architecture, no classroom.

---

## Places, ranked

**OSR Online / NTDEV** — where Windows driver developers actually are. Display
and RDS questions get answered by people who have shipped these. Slow but
authoritative.

**Microsoft Q&A**, `windows-hardware-drivers` — mixed quality, occasionally an
actual Microsoft engineer.

**The VDD project's Discussions** — they ship a working IddCx driver, so the
IddCx questions land well there. Less useful for the RDS provider, which is a
different area.

**Stack Overflow** — worth a try for the `DisconnectReason` mapping alone. It is
the sort of thing someone has already written down.

`IWRdsProtocolConnection` is a thin, sparsely documented corner of Windows.
Expect few answers, and expect the good one to come from someone who worked on
RDS or shipped a thin client.
