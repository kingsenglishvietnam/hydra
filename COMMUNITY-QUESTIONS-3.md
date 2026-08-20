# Questions round 3 — GetInputHandles wants kernel input devices

The problem has changed shape again, and the questions with it.

`GetInputHandles` is documented as returning **a handle to an I8042prt keyboard
driver** and **a handle to a Mouclass driver**. Not pipes, not events — handles
to kernel input device objects. Only `pBeepHandle` is documented as legitimately
NULL.

That means a protocol provider is expected to bring its own virtual input
device stack, the way RDP has `TermDD`/`RdpBus` alongside `RdpIdd`. Mode 4 would
need three drivers, not one.

**Unless termsrv only needs the handles to be valid rather than functional.**
Hydra already delivers keyboard and mouse through Interception and `SendInput`;
it does not need termsrv's input path to carry anything. That distinction is the
single highest-value thing to establish, and it is what these questions are for.

All are postable without identifying the project.

---

## The question that decides everything

**Where:** OSR Online, NTDEV.

> ### IWRdsProtocolConnection::GetInputHandles — does termsrv require functional input devices, or merely valid handles?
>
> Windows 11 24H2. Custom RDS protocol provider, registered as a `WinStations`
> listener. It creates a real session: authentication succeeds (4624, Logon Type
> 10), the shell process starts, and a remote-session IddCx driver attaches and
> presents a monitor. About 400 ms after `NotifyCommandProcessCreated`, termsrv
> calls `PreDisconnect` with reason 18 and tears the session down.
>
> `GetInputHandles` currently returns `S_OK` with all three out-params set to 0
> — that is what the WDK sample does, with a `//TODO: Provide sample
> implementation`.
>
> The documentation says `pKeyboardHandle` is "a handle to an I8042prt keyboard
> driver" and `pMouseHandle` is "a handle to a Mouclass driver". So I take it a
> provider is expected to supply its own virtual input device stack.
>
> **My question is whether that is strictly required in my case.** My session
> receives its keyboard and mouse from outside the RDS path entirely — an
> in-session helper injects with `SendInput`. I need termsrv to keep the session
> alive; I do not need it to deliver input.
>
> 1. Does termsrv **use** these handles, or does it only validate and store
>    them? Is returning 0 what causes reason 18, or is that a coincidence of
>    timing?
> 2. If handles are required, will handles to the **existing** `\Device\
>    KeyboardClass0` / `\Device\PointerClass0` objects satisfy it, or must they
>    be device objects the provider owns?
> 3. Is there a documented minimal virtual input device for this purpose, or is
>    every provider expected to ship keyboard and mouse miniport drivers
>    alongside the protocol DLL?
> 4. Does `IWRdsProtocolConnection` have a supported "no input" mode — a
>    property via `QueryProperty`, or a capability flag — for providers whose
>    sessions get input by other means?
>
> I would rather know it needs three drivers than discover it after writing two.

The reason-code progression is worth including: **17 with a broken IDD at
~1.2 s, 12 with no IDD at ~33 s, 18 with a working IDD and null input handles at
~0.4 s.** It shows each fix moved the failure.

---

## The question that might sidestep it

> ### Is there a supported way to have RDS create a session without a protocol provider owning the input path?
>
> I need a second interactive Windows session on a client SKU, with a display,
> whose input arrives from outside RDS. The session lifecycle is the only thing
> I need from termsrv.
>
> Is there any supported route to that other than a full protocol provider? For
> example:
>
> - Can a provider delegate the input path back to termsrv's own devices,
>   rather than supplying its own?
> - Is `IWRdsProtocolConnection` the only interface that can bring up a session,
>   or is there a lighter-weight path?
> - Do the console-session APIs (`WTSConnectSession`, session 0 isolation
>   plumbing) offer anything here?
>
> I am aware client SKUs are licensed for one interactive session and am not
> asking how to defeat that; this is a single-user machine and the second
> session is for the same person.

That last sentence matters. Without it the question reads as license
circumvention and gets no answers.

---

## The prior-art question, now more pointed

> ### Do thin-client protocol providers ship their own keyboard and mouse miniports?
>
> Given `GetInputHandles` is documented as returning I8042prt and Mouclass
> handles, I assume every third-party RDS protocol provider ships virtual input
> drivers alongside the protocol DLL. Is that right?
>
> If so, is there a canonical minimal example of such a driver — even a
> reference to which WDK sample is the right starting point? `kbfiltr` and
> `moufiltr` are filters rather than the device objects a provider would own,
> and I cannot find a sample that creates one.

If the answer is "yes, and here's the sample", the project becomes tractable
again. If it is "yes, and everyone writes their own from scratch", that is worth
knowing before starting.

---

## The one to ask Microsoft

**Where:** Microsoft Q&A, `windows-server-rds`.

> ### Documentation gap: what does termsrv do with the handles from GetInputHandles?
>
> The page documents the parameter types but not the contract — whether termsrv
> writes to these handles, when, what happens if they are NULL, or what the
> minimum viable implementation is. The WDK sample leaves it as a TODO.
>
> Related: `PreDisconnect`'s `DisconnectReason` has no documented value mapping.
> Observed 12, 17 and 18 in distinct, reproducible failure modes.
>
> Both gaps make provider development largely guesswork. Is there internal
> documentation that could be published, or a sample that implements the input
> path?

---

## How to frame it now

**Lead with what works.** A remote-session IddCx driver initialising with
`caps.Flags=0x5` and presenting a monitor is a strong opening. It establishes
you are past the part most people get stuck on.

**Be explicit that you do not need the input path to work.** That is the whole
question, and it is unusual enough that people will otherwise answer the
question you did not ask.

**Include the sample's TODO verbatim.** That the WDK itself leaves this
unimplemented is context that helps.

**Say it is a single-user machine.** Otherwise a second interactive session on a
client SKU reads as licensing circumvention, and you get silence.

---

## Before posting, one thing worth trying

If anything in your input stack already opens a kernel input device — the
Interception layer, or a filter — those handles cost nothing to pass:

```powershell
Get-ChildItem C:\Programs\hydra -Recurse -Include *.c,*.cpp,*.h | Select-String -Pattern 'KeyboardClass0|PointerClass0|Device\\\\Keyboard|Device\\\\Pointer' | Select-Object Path, LineNumber, Line
```

A ten-minute experiment: open `\Device\KeyboardClass0` and `\Device\
PointerClass0` with `CreateFile`, return those handles, and see whether termsrv
accepts them. It either lives or it does not, and either answer is worth having
before writing a driver.

Failing that, `IoCreateDevice` with the right device type in a minimal KMDF
driver is the shape — but confirm it is required before building it.
