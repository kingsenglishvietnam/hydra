# Follow-up questions — writing the transport half

The situation has narrowed. The driver works. The bypass is closed. What remains
is a protocol provider that holds its own session, and these are the questions
that would make that a few hundred lines rather than a few thousand.

All are postable without identifying the project.

---

## What we now know, and how it changes the asking

**The driver is done.** A remote-session IddCx driver loads, initialises with
`caps.Flags = IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER | USE_SMALLEST_MODE`,
and presents a 1920x1080 monitor. Three clean cycles.

**The DXGI bypass is closed, and this was tested rather than assumed.** With a
healthy mode-2 session capturing normally, killing the RDP client makes the
session go `Disc`, `EnumOutputs` return nothing, and Desktop Duplication stall
immediately. **`RdpIdd_IndirectDisplay` is torn down with the connection.** It is
not a persistent display that happens to be fed by a client; it exists only
while the connection is active. So duplicating the in-box remote IDD directly is
exactly what a session-resident capture helper already does, and it cannot
outlive the client.

That leaves one route: a provider that keeps its session alive on its own.

---

## The main question

**Where:** OSR Online, NTDEV. This is the one that decides whether the remaining
work is days or weeks.

> ### IWRdsProtocolConnection: what does a minimal transport have to actually service?
>
> Windows 11 24H2. I have a custom RDS protocol provider based on
> `TestProtocol_Ext`, registered as a `WinStations` listener. It creates a real
> session — authentication succeeds (4624, Logon Type 10), the shell process
> starts, and a remote-session IddCx driver attaches and presents a monitor.
>
> ~400 ms after `NotifyCommandProcessCreated`, termsrv calls `PreDisconnect`
> with reason 18, then `DisconnectNotify` and `Close`. I understand this to mean
> termsrv has decided the transport is unresponsive — the sample has no I/O
> loop and returns stub values from the status methods.
>
> I am writing the transport half. Before I do, I would rather know the shape
> than discover it:
>
> 1. **`GetInputHandles`** — what are the handles, concretely? Named pipes,
>    events, section handles? Who creates them, me or termsrv? What is the wire
>    format of what termsrv writes into them, and is there a documented header
>    or is it opaque `WTS` input records?
> 2. **What must be drained, and how promptly?** Is there a buffer size or a
>    timeout after which termsrv treats a full pipe as a dead client? Reason 18
>    arrives at 400 ms, which suggests something quite tight.
> 3. **`GetProtocolStatus`** — which fields of `WTS_PROTOCOL_STATUS` does
>    termsrv actually read to decide a session is alive? Is
>    `Output.ProtocolType` / the counters enough, or does it want monotonically
>    increasing byte counts?
> 4. **`GetLastInputTime`** — is this consulted for the idle disconnect, and
>    does it need to advance even when there is genuinely no input?
> 5. **Is there a keepalive callback I should be calling**, rather than only
>    responding? Something on `IWRdsProtocolConnectionCallback`?
>
> The goal is a provider that owns a session with no wire protocol at all — the
> "client" is local and there is nothing to transmit. If that is a supported
> shape I would like to know what the minimum viable set of live methods is.

Include the provider log verbatim. The reason-code progression is the strongest
part: **17 with a broken IDD at ~1.2 s, 12 with no IDD at ~33 s, 18 with a
working IDD at ~0.4 s.** It shows the problem moved as things were fixed.

---

## The question that might make the transport unnecessary

Worth asking alongside, because a yes changes everything.

> ### Is there a supported way to keep an RDS session's indirect display alive without an active connection?
>
> `RdpIdd_IndirectDisplay` creates a devnode per session under
> `SWD\REMOTEDISPLAYENUM\`. I have confirmed by testing that this display is
> destroyed when the connection ends — `EnumOutputs` returns nothing and Desktop
> Duplication stalls within seconds of the client exiting.
>
> Is there any supported mechanism by which a session retains a display after
> disconnect? Specifically:
>
> - Does `WTSDisconnectSession` behave differently from the client simply
>   exiting, with respect to the session's IDD?
> - Is there a policy or `WinStation` property that keeps a disconnected
>   session's display stack instantiated?
> - Do RDS session collections on Server SKUs behave differently here, and if
>   so is that a licensing/SKU gate or a code path difference?
>
> I am aware "disconnected sessions have no display" is the intended design. I
> am asking whether there is a supported exception, not looking to defeat it.

A yes here would mean ordinary RDP creates the session, the client exits, and
the display persists — no custom provider at all.

---

## The cheaper architectural question

> ### Minimal RDS protocol provider — is anyone shipping one, and is there prior art?
>
> Is there a public example of an `IWRdsProtocolConnection` implementation that
> maintains a session, beyond the WDK's `TestProtocol_Ext` scaffolding? Thin
> client vendors and remote-desktop products implement this interface, so
> presumably the shape is known even if the code is not.
>
> Failing that: roughly how much of the interface is genuinely required for a
> session that never transmits anything? The interface is large and the sample
> stubs almost all of it, which makes it hard to tell what is ceremony and what
> is load-bearing.

Prior art would save the most time of anything here.

---

## One to ask Microsoft specifically

**Where:** Microsoft Q&A, `windows-hardware-drivers` or `windows-server-rds`.

> ### Documentation gap: DisconnectReason values passed to IWRdsProtocolConnection::PreDisconnect
>
> The parameter is documented as `ULONG DisconnectReason` with no value mapping.
> Observed values 12, 17 and 18 in different failure modes, each meaningful and
> reproducible. Is there a public enumeration, or a header these come from?
>
> Without it, every provider failure is indistinguishable from every other one
> and diagnosis is guesswork.

Small, specific, and the sort of thing a documentation team can act on.

---

## How to frame it now

**Lead with the driver working.** It changes the reception entirely. "I have a
remote-session IddCx driver initialising with `caps.Flags=0x5` and presenting a
monitor; what I need is the transport" is a question from someone who has done
the work.

**Include the negative result on the bypass.** That you *tested* whether the
in-box IDD survives client exit, and it does not, saves anyone suggesting it and
shows the reasoning is empirical.

**The reason-code progression is your best evidence.** 17 → 12 → 18 as different
things were fixed is a diagnostic sequence, not a symptom.

**Ask for shape, not code.** "What must be serviced and how promptly" is
answerable in a paragraph. "How do I write this" is not.

---

## Realistic expectations

`IWRdsProtocolConnection` is thin, sparsely documented territory. The people who
know it worked on RDS at Microsoft or shipped a thin client. There may be few
replies, and the useful one may take a week.

Ask the bypass question too. It is cheap, and a supported way to keep a
disconnected session's display alive would remove the need for any of this.
