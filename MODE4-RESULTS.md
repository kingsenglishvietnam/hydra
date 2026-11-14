\# MODE4-RESULTS.md — what each mode 4 run measured
Companion to `MODE4-RETEST-GUARD.md` (the procedure). Kept separate so the
runbook stays a runbook and the findings accumulate.
\## Run B, 2026-11-12
\*\*Setup.\*\* Build 26300.9974. termsrv.dll on disk reports
`FileVersionRaw=10.0.26100.9818`, SHA256 matches `termsrv-guard.json`
`PatchedSha`. Byte-diff against the archived 9818 original: \*\*one byte\*\*,
`0xA9787: 0x75 → 0xEB` (page 169). Canonical cap-lift, otherwise identical.
Remote IDD (`oem232.inf` / `iddseat-remote.inf`) staged before today. Provider
rebuilt with `HydraProvLog` instrumentation and the newline fix.
\### Result: reason=18, 4 ms after the display arrived
Timeline (provider log + DriverFrameworks-UserMode + `C:\\HydraLog\\iddseat.log`):

| t (sec) | source | event |
|---|---|---|
| 43.793 | provider | `EnableWddmIdd(1)` |
| 43.795 | provider | `AcceptConnection`, `GetClientData` |
| 43.798 | provider | `AuthenticateClientToSession` |
| 43.905 | provider | `NotifySessionId` |
| 43.905 | provider | `GetInputHandles` — returned null (see below) |
| 43.906 | provider | `GetHardwareId cap=200` |
| 43.906 | provider | `GetHardwareId -> hr=0x00000000, len=22` |
| 43.943 | provider | `ConnectNotify session=3` |
| 43.955 | UMDF | host process starting for `HYDRASEAT\_REMOTEIDD\_V1\&SESSIONID\_0003` |
| 44.077 | UMDF | driver loaded at level 0 |
| 44.080 | driver | `IddCxAdapterInitAsync -> 0x00000000` |
| 44.095 | driver | `AdapterInitFinished AdapterInitStatus=0x00000000` |
| 44.096 | driver | `IddCxMonitorCreate -> 0x00000000` |
| 44.100 | driver | `IddCxMonitorArrival -> 0x00000000` |
| 44.104 | provider | `PreDisconnect reason=18` |
| 44.142 | provider | `OnDriverUnload session=3` |
| 44.145 | provider | `DisconnectNotify`, `Close` |

\### What this establishes
1\. \*\*The cap is not the cause.\*\* One-byte patch, same outcome. If the cap
&#x20;  mattered, this run would have gone differently from the stock runs of
&#x20;  08-20 and it didn't.
2\. \*\*The driver is not the cause.\*\* Every IDDCX step through monitor arrival
&#x20;  returns `0x00000000` on this build. Reason=17 (driver failed to load) is
&#x20;  ruled out; reason=12 (hardware ID mismatch) is ruled out.
3\. \*\*Termsrv made the decision.\*\* `PreDisconnect` is a termsrv to provider
&#x20;  callback, not a provider to termsrv call. The provider's subsequent calls
&#x20;  (`OnDriverUnload`, `DisconnectNotify`, `Close`) are reactions.
\### What this does not establish
\*\*`OnDriverLoad` never fires.\*\* It is instrumented as a bare log call
(`HydraProvLog(...)` + `return S\_OK`), so absence means termsrv did not
invoke it. Two readings fit the data:
\- Termsrv disconnects because there is no input path — it is checking
&#x20; `GetInputHandles`-provided state against the loaded display, and the
&#x20; handles are null. The 4 ms gap between monitor arrival and
&#x20; `PreDisconnect` is termsrv doing that check.
\- `OnDriverLoad` is a post-success notification, not a precondition.
&#x20; Termsrv disconnects for a reason not visible from any current log.
The synthetic-handle experiment (below) distinguishes these.
\*\*`GetInputHandles` has never had a defined behavior on this machine.\*\*
The sample shipped with `pKeyboardHandle = NULL;` — reassigns the local
parameter, writes nothing through the pointer. Before the fix in the
instrumentation commit, termsrv received whatever was in its stack slots:
null if it pre-zeroed, garbage otherwise. `cfe20f4` records "null handles
from GetInputHandles produce reason=18" — but whether that was a
measurement or a code reading is not visible from this repo, and if it
was a measurement, what was measured may have been termsrv's own
initialization, not anything the provider returned.
`NotifyCommandProcessCreated` also never fires. Not a signal.
\### Next experiment: log what termsrv passed in
Add before the current fix runs:
```cpp

HydraProvLog("GetInputHandles -> in kbd=%p mou=%p beep=%p",
&#x20;            (void\*)\*pKeyboardHandle, (void\*)\*pMouseHandle, (void\*)\*pBeepHandle);

## Run A, 2026-11-12 21:33 — driverless, reframes run B above

**Read this before drawing conclusions from the run B section.** Run A was
performed AFTER run B. It changes what run B means.

Setup: same patched 9818, no IDD staged (`oem232` removed, `oem231` console
seat still present). Same provider, same patched termsrv.

### Result: full desktop session, 60-second lifetime, reason=15 timeout

Provider log (21:33:20):
- 20.674 EnableWddmIdd(1)
- 20.674 AcceptConnection, GetClientData, AuthenticateClientToSession
- 20.724 NotifySessionId
- 20.724 GetInputHandles  (returned null — same as run B)
- 20.724 GetHardwareId cap=200 -> hr=0x0, len=22
- 20.737 ConnectNotify session=2
- **20.907 NotifyCommandProcessCreated session=2**  (never fires in run B)

LSM events:
- 21.687 Begin session arbitration
- 21.992 Session logon succeeded
- 22.474 Shell start notification received

RCMM events:
- 20.674 Listener HydraProto received a connection
- 22.502 WDDM graphics mode is enabled

Sampler (2-second cadence):
- t+6s  id=2 Active teacher, 17 procs
- t+11s **explorer=True**, 30 procs, dwm=True
- t+11s..t+32s Active, explorer=True throughout, 24-33 procs
- Session silent from t+67s (dead before next sample at t+34s)

PreDisconnect:
- 21:34:20.728 PreDisconnect reason=15  (64.7s after trigger)
- 21:34:20.729 LSM: Session 2 disconnected, reason code 15

### What this establishes

1. **The provider path creates a real desktop session.** 08-11 reproduced.
   Patched termsrv + custom provider + no IDD = login shell, Explorer, DWM,
   30 processes. Not a stalled session. A working one.

2. **`cfe20f4` is refuted as stated.** "Null handles from GetInputHandles
   produce PreDisconnect reason 18" — the handles returned null in run A
   too, and the session ran 60 seconds. Null handles ALONE do not cause
   reason 18. If they contribute, they only do so in combination with the
   IDD arrival.

3. **The IDD arrival is what triggers reason=18.** Run A and run B differ
   in exactly two things: IDD presence (`oem232` staged or not) and outcome.
   Every other input — patched termsrv, null handles, provider sample,
   credentials, session creation — is identical.

4. **`NotifyCommandProcessCreated` fires only without IDD.** In run B the
   session never reaches userinit; the disconnect happens before the shell
   process is created. That absence is downstream of the disconnect, not a
   cause of it.

### The 60-second reason=15 — a separate finding

Reason=15 at exactly 60s after trigger. Different class of failure from
reason=18. Likely the sample's `WaitToConnect` does not send periodic
callbacks, so termsrv times out the session. Needs source reading of
`WRdsProtocolListener.cpp` and the `IWRdsProtocolListenerCallback` interface
to see what the sample calls after connection establishment. Not blocking:
the shell came up, so the initial handshake works. The timeout is a
keepalive issue.

### What this does to the kernel-driver plan

`KERNEL-DRIVER-PLAN.md` said: "termsrv's session cap blocks the provider
path; a WDDM miniport bypasses it." That was wrong. Termsrv does not block
provider sessions. It blocks them **when an IDDCX driver arrives.**

The kernel display path is still a candidate — because a WDDM miniport is
not IDDCX, and termsrv's check might be IDDCX-specific. But that is a
hypothesis with real cost. There is a cheaper test first.

### Next experiment: synthetic handle test

Before any kernel work, test whether the IDD-arrival disconnect is caused
by the null input handles:

1. Log what termsrv passed in — `GetInputHandles -> in kbd=%p mou=%p beep=%p`
   before the nulling fix in `WRdsProtocolConnection.cpp`.
2. Flag-gated return of a real `CreateEventW` handle when the flag file
   `C:\ProgramData\Hydra\input-nonnull.flag` is present.
3. Rebuild, run B3 with IDD staged and the flag present.

Outcomes:
- **Session holds** -> input path is the fix. Kernel display driver
   unnecessary. Virtual input driver is the next build.
- **reason=18 again** -> handles exonerated even with IDD present. Kernel
   display path becomes the only remaining option.

Two runs, one rebuild, ~30 minutes.

## B3 / B4 — the ordering race, 2026-11-12 late

Same binary, same inputs. B3 survived 108s. B4 died at 2.2s. The variable
is ordering of MonitorArrival vs. Session logon:

- B3: MonitorArrival at 15.465, logon succeeded at 15.864 (399ms BEFORE logon)
- B4: MonitorArrival at 03.843, logon succeeded at 03.703 (140ms AFTER logon)

Termsrv accepts a session where the display arrived before logon. Termsrv
rejects one where the display arrived after logon with ConfigUpdate2 failing
0xC000000D. The failure to commit a valid config makes termsrv's post-logon
check reject.

Fix: make IddCxAdapterDisplayConfigUpdate2 succeed. Then ordering stops
mattering. Candidates in order:
1. VSyncFreqDivider = 0 (currently 1, MS reference uses 0)
2. Remove MonitorColorMode or add its valid flag

B3's 108s survival is not reproducible under the current code. The 108s is
not a working state; it's a lucky ordering.

## B3–B6 — ConfigUpdate2 failure and the reason=18 stall

Same binary across these except as noted. All runs: `IddCxGetVersion -> 0x10C00` (runtime IddCx 1.12), headers 1.10 (kit 26100).

### Timeline

| run | MINOR | VSync | AdapterInitFinished | ConfigUpdate2 | reason=18 |
|---|---|---|---|---|---|
| B3 | 2 | 1 | reached | 0xC000000D | never (lucky, 108s) |
| B4 | 2 | 1 | reached | 0xC000000D | yes (2.2s) |
| B5 | 10 | 0 | NOT reached (stall) | never called | yes (1.6s) |
| B6 | 2 | 0 | reached | 0xC000000D | yes (1.6s) |

### What B3–B6 establishes

1. **B3's 108s survival was an ordering fluke, not a fix.** B4 was the same binary and died at 2.2s. Nothing about the code changed between them.
2. **MINOR=10 stalls the async adapter init.** `IddCxAdapterInitAsync` returns `0x0` but `AdapterInitFinished` never fires. Runtime is 1.12; the 1.10 build doesn't complete the async handshake. MINOR=2 works. MINOR=10 does not.
3. **VSyncFreqDivider=1 vs =0 makes no difference.** Both return `0xC000000D`. That field is eliminated.
4. **ConfigUpdate2 failure is the constant.** It's present in every run that reaches that call. It's the standing cause.

### Next candidates (in order)

1. **MonitorColorMode** (line 663). The field is set to SDR but `IDDCX_DISPLAYCONFIGPATH2_FLAGS_MONITOR_COLOR_MODE_VALID` is not set in `path.Flags`. If the runtime validates "non-default MonitorColorMode requires the flag", it rejects. Fix: remove the line (leave default) or add the flag.
2. **MINOR=12** (match runtime). Higher risk — MINOR=10 already stalled. Try only if MonitorColorMode fails.
3. Another field of `IDDCX_DISPLAYCONFIGPATH2` that's set wrong for 1.12.

### End-of-session state (2026-11-12 late)

Working tree:
- `build-driver.ps1`: `IDDCX_VERSION_MINOR=2`
- `iddseat.cpp:647`: `path.Mode.VSyncFreqDivider = 0`

Both committed in the next commit so tomorrow starts from a known baseline.

Installed: `oem223.inf iddseat-remote.inf`, dist INF `DriverVer = 11/12/2026,1.6.0.3`.

B6 log saved at `logs/mode4-guard-B6-20261112-223447.txt`. B5 at `logs/mode4-guard-B5-20261112-222648.txt`.

## B7 and B8, 2026-11-13 — scale factor test

### B7 (13:43): state failure, driver never loaded

reason=17 at 490ms after ConnectNotify. No `IddCxAdapterInitAsync` in the log because
`C:\HydraLog\iddseat.log` was never created — `DriverEntry` did not run. DriverFrameworks
shows no `id=1003` for the remote IDD. No `Begin session arbitration` event in LSM.
Different failure class from every prior run. Not explained; didn't reproduce in B8.

### B8 (13:50): full driver init, scale factor still fails

Complete driver log:

```
13:50:51.376 IddCxGetVersion -> 0x00000000, IddCxVersion=0x10C00
13:50:51.378 IddCxAdapterInitAsync -> 0x00000000
13:50:51.382 AdapterInitFinished: entered, AdapterInitStatus=0x00000000
13:50:51.383 IddCxMonitorCreate -> 0x00000000
13:50:51.385 QueryTargetModes: entered, inputCount=0
13:50:51.385 QueryTargetModes: entered, inputCount=1
13:50:51.385 IddCxMonitorArrival -> 0x00000000
13:50:51.386 IddCxAdapterDisplayConfigUpdate2: calling, 1920x1080@60 flags=0x3 scale=100 size=136
13:50:51.388 IddCxAdapterDisplayConfigUpdate2 -> 0xC000000D
13:50:51.389 AdapterInitFinished: returning 0x00000000
```

`PreDisconnect reason=18` at 13:50:51.389, 14ms after `AdapterInitFinished` entered.

### What B8 establishes

1. **The scale-factor patch is live.** `flags=0x3 scale=100 size=136` — MODE_VALID
   and MONITOR_SCALE_FACTOR_VALID both set, MonitorScaleFactor=100.

2. **Query ordering was already correct.** `QueryTargetModes` fires twice (inputCount=0
   then inputCount=1) before the config update. The "call is too early" hypothesis
   is refuted.

3. **Scale factor was not the cause.** With the documented first-call requirement met,
   `ConfigUpdate2` still returns `0xC000000D`.

4. **Struct size the driver sends: 136 bytes.** Runtime IddCx 1.12 (`0x10C00`).
   Compile MINOR=2. Header path is 1.10 per build-driver output.

### Candidates remaining (in order of cheapness)

- **A:** diagnose why `IDDCX_VERSION_MINOR=10` stalls `AdapterInitFinished` (B5) —
  read the header diffs between 1.0/1.2/1.10, check `IDDCX_ENDPOINT_VERSION`,
  check MS IddSampleDriver.
- **B:** test `IDDCX_VERSION_MINOR=12` (match runtime exactly).
- **B′:** `idd-skip-config-update.flag` — tests whether `ConfigUpdate2` is needed at all.
- **C:** compare against Microsoft's IddSampleDriver on the same build.

If MS's sample doesn't call `ConfigUpdate2` and works, the answer is A or B′,
not C.

## B10 and B11, 2026-11-13 — config call exonerated, input handle type-checked

### B10-skipconfig (14:22): not calling ConfigUpdate2 also ends in reason=18

Flag C:\HydraLog\idd-skip-config-update.flag present. Driver log:

```
14:22:11.581 ConfigUpdate2: skip flag PRESENT (err=0)
14:22:11.581 IddCxAdapterDisplayConfigUpdate2 SKIPPED
14:22:11.581 AdapterInitFinished: returning 0x00000000
```

PreDisconnect reason=18 90ms after ConnectNotify.

**What this shows:** the *act* of calling IddCxAdapterDisplayConfigUpdate2 is
not what kills the session. Not making the call produces the same reason=18
outcome as making it and failing.

**What this does not show:** that a *successful* config call would still end in
reason=18. Skipping and failing the call leave the session in the same state —
IDD present, no active display path. A successful call would produce a
*different* state, and that condition has never been tested.

The August 0xD000000D from the ConfigUpdate2 call is 0xC000000D with bit 28
set — STATUS_INVALID_PARAMETER. See decode note below.

### B11-synthetic (14:36): non-null event rejected at type check, before IDD

Flag C:\ProgramData\Hydra\input-nonnull.flag present. GetInputHandles returns
a real CreateEvent handle for all three slots.

```
GetInputHandles -> in kbd=0000000000000000 mou=0000000000000000 beep=0000000000000000
GetInputHandles -> out kbd=0000000000000864 mou=0000000000000864 beep=0000000000000864 mode=synthetic
```

Termsrv never calls ConnectNotify. IDD host never starts. Failure:

```
id=20 Attempt to send connect (5) message to Windows video subsystem failed. Status 0xD0000024.
id=36 An error occurred when transitioning from CsrConnected in response to EvCsrInitialized. (ErrorCode 0xD0000024)
id=40 Session 5 has been disconnected, reason code 12
```

0xD0000024 with bit 28 cleared is 0xC0000024 — STATUS_OBJECT_TYPE_MISMATCH.
Confirmed against
C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared\ntstatus.h
line 2411.

**What this shows:**

- termsrv passes NULL for all three handle slots — measured, not inferred
- null values pass the video subsystem's connect check
- an event handle is the wrong kernel object type; the video subsystem takes
  the value, references it as a specific object type, and the kernel says
  STATUS_OBJECT_TYPE_MISMATCH
- the failure is pre-IDD: ConnectNotify never fires, the driver never loads

**What this does not show:**

- that null handles *cause* reason=18. The B11 run doesn't reach the
  IDD-arrival code path at all; it fails before ConnectNotify.
- which of the three handle slots was rejected. All three got the same event,
  including beep, which the docs say may be null. A future test must set one
  slot at a time.

### Decode note for 0xD000xxxx status codes

Bit 28 is the marker Windows sets when it wraps an NTSTATUS as an HRESULT. It
is not part of the facility field. Clear it and the standard NTSTATUS layout
applies:

```
0xD0000024 -> 0xC0000024 -> STATUS_OBJECT_TYPE_MISMATCH
0xD000000D -> 0xC000000D -> STATUS_INVALID_PARAMETER
```

Check any new 0xD000xxxx code against shared\ntstatus.h with bit 28 cleared.

### What this leaves open

Two conditions remain untested:

1. **A correctly typed input handle.** An event is the wrong type. What
   handle *would* pass the type check, and whether it changes what termsrv
   does at IDD arrival, is untested. Cheapest path is the same flag with a
   file object or similar. **Run on a VM, not the teaching machine** — the
   risk is a handle of the right *type* that points at a device which isn't
   a keyboard or mouse. win32k may treat it as the device type the handle
   advertises and attempt operations on a device that doesn't respond. Test
   one slot at a time.

2. **A config call that returns 0x0.** The remaining suspect is the version
   skew: IDDCX_VERSION_MINOR=2 declares the driver as IddCx 1.2, runtime is
   1.12, header kit is 1.10. MINOR=10 (B5) stalled AdapterInitFinished —
   unexplained. Understanding that stall is a prerequisite to testing
   MINOR=10 or MINOR=12 against the call.

### Status: open, paused

Mode 4 is not closed. The two untested conditions above are research
problems — a VM, a few mornings, and the answer may not change the outcome.
The design decision (separate from the research) is whether that's worth
spending, given mode 7 already delivers the classroom value.

### B3 as the exception to note

Run B3 (2026-11-12 21:57) had the same failing ConfigUpdate2 call as every
other IDD-attached run, but the session lived 108 seconds. B4 with the
identical binary (store hash 26B90E7565124820) died at 2.2s. B3's survival
has never been reproduced and remains unexplained. **The pattern is "failing
config usually ends in reason=18"; B3 is the exception.**

## Phase A, 2026-11-13 late — MINOR=10 investigation (partial)

Read-only header investigation into why MINOR=10 stalls AdapterInitFinished.
Time-boxed at 2h; stopped before reaching a conclusion.

### Findings

**Header versions bundled with kit 10.0.26100.0:** 1.0, 1.2, 1.3, 1.4, 1.6,
1.7, 1.8, 1.9, 1.10. **No 1.11 or 1.12.** MINOR=12 is not a valid build
target — the "try MINOR=12 to match runtime" item from earlier workflows was
wrong.

**MS's IddSampleDriver build pattern:** /DIDDCX_VERSION_MINOR=6
/DIDDCX_MINIMUM_VERSION_REQUIRED=4. Targets 1.6 features, requires framework
1.4 to load. The separation enables runtime availability checks.

**Hydra's pattern:** IDDCX_VERSION_MINOR=2 only.
IDDCX_MINIMUM_VERSION_REQUIRED defaults to IDDCX_VERSION_MINOR per
IddCxFuncEnum.h, which sets IDD_EVERYTHING_ALWAYS_AVAILABLE and makes all
function calls direct (no runtime gating). Hydra should adopt MS's
MINIMUM < MINOR pattern eventually; not blocking for the current failure.

**0xC000000D reason is WPP-only.** Header doc for
IddCxAdapterDisplayConfigUpdate2 says the reason will be "logged using WPP."
WPP is Microsoft-side — not enablable from a driver. The specific reason
STATUS_INVALID_PARAMETER is returned cannot be observed.

### What this changes

The config-call line of investigation is closed by B10 regardless. Phase A
confirms there is no productive next step for it:

- No way to enable the WPP diagnostic
- No newer header to bump to
- No clear fix path even if one existed

MINIMUM/MINOR build hygiene is worth doing but does not advance the
reason=18 problem.

### The only remaining test

Typed input handle. A file object or HID-like handle instead of an event.
Requires a VM — the risk is a right-type handle pointing at a non-keyboard
device, which win32k may attempt operations on. Set one slot at a time
(B11 set all three to the same event, so we do not know which was rejected).

### MINIMUM/MINOR correction (same session)

An attempt to adopt MS's MINIMUM<MINOR pattern failed to compile:

  IddCxFuncEnum.h(47): #error: IDDCX_MINIMUM_VERSION_REQUIRED > IDDCX_VERSION_MINOR

The header requires MINIMUM >= 3 AND MINIMUM <= MINOR. At MINOR=2 the pattern
is structurally impossible. Adopting it requires bumping MINOR to at least 3.

Open observation: at MINIMUM == MINOR (the default) the driver calls IDD
functions by direct table index. At MINIMUM < MINOR the driver checks
FunctionName##TableIndex < IddFunctionCount before each call. B5's MINOR=10
stall might be function-table drift between the 1.10 stub library and the
1.12 runtime; if so, MINOR=10 + MINIMUM=3 would either fix it or fail loudly
via WdfDriverErrorReportApiMissing rather than silently hanging. Testable,
but not pursued - config call is closed by B10 and this doesn't reopen it.


## B13-B15, 2026-11-13 — null handles eliminated as the reason=18 cause

Flag-gated File object test in GetInputHandles, one slot per run.

### Results

| run | slot with file | ConnectNotify | IDD level 0 | reason |
|---|---|---|---|---|
| B13 | kbd  | out kbd=0x86C | yes | 18 |
| B14 | mou  | out mou=0x868 | yes | 18 |
| B15 | beep | out beep=0x874 | yes | 18 |

All three: `GetInputHandles -> in kbd=0x0 mou=0x0 beep=0x0`, single slot
non-null on the out line, `ConnectNotify` fires, IDD loads to level 0,
`AdapterInitFinished` enters, `MonitorArrival -> 0x0`, `reason=18`.

### What this establishes

The video subsystem accepts File objects in all three slots. The session
reaches IDD arrival with a non-null handle and dies at reason=18 anyway.
**Null handles are not the cause of reason=18.**

Combined with B11 (event handle rejected at STATUS_OBJECT_TYPE_MISMATCH):
- Null: passes
- File: passes
- Event: rejected

So the check is a type check that File satisfies. The reason=18 disconnect
is downstream of that check.

### What this implies about the August kernel-driver conclusion

A kernel keyboard/mouse driver's device handle, obtained via CreateFile on
the device interface, is also a File object. B13-B15 passed that type check
the same way a real device handle would. If reason=18 were about the type
check, B13-B15 would have held. They did not.

This does not refute August — it refutes the I8042prt/Mouclass framing of
that conclusion. Either the reason=18 decision is downstream of the type
check and only passes with a real device that responds to IOCTL, or the
decision is unrelated to input handles entirely.

### Where this leaves mode 4

Open, paused. The four handle-content hypotheses are eliminated:
- VSyncFreqDivider (B6)
- Scale factor (B8)
- ConfigUpdate2 invocation (B10)
- Null vs non-null at the type check (B13-B15)

The reason=18 decision remains unattributed at this instrumentation level.

## B16, 2026-11-13 evening — Procmon trace of the file handle in the keyboard slot

Filtered Procmon capture on a B13-style run (`input-file-kbd.flag`, file
handle returned as `kbd=0x834`). Three-log timeline:

```
20:10:29.538  provider   GetInputHandles (entry)
20:10:29.539  procmon    svchost CreateFile hydra-file-input.txt SUCCESS
20:10:29.540  provider   GetInputHandles -> out kbd=0x834 mode=file-kbd
20:10:29.541  provider   GetHardwareId -> hr=0x0
20:10:29.559  provider   ConnectNotify session=3                (+20ms)
20:10:29.573  procmon    csrss DeviceIoControl 0xb0000 -> INVALID PARAMETER  (+34ms)
20:10:29.573  procmon    csrss QueryDeviceRelations SUCCESS
20:10:29.708  iddseat    DriverEntry                           (+135ms after IOCTL)
20:10:29.782  iddseat    IddCxMonitorArrival -> 0x0
20:10:29.793  iddseat    ConfigUpdate2 -> 0xC000000D
20:10:29.795  iddseat    AdapterInitFinished returning 0x0
20:10:29.807  provider   PreDisconnect reason=18               (+25ms after MonitorArrival, +234ms after IOCTL)
20:10:29.807  procmon    csrss CloseFile                       (same ms as PreDisconnect)
```

`0xb0000` decodes as `CTL_CODE(FILE_DEVICE_KEYBOARD=0xb, Function=0x0000,
METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)` = IOCTL_KEYBOARD_QUERY_ATTRIBUTES.
Verified against `ntddkbd.h`.

### What this establishes

- Win32k (`csrss.exe`) probes the handle as a keyboard, not termsrv.
- The probe is QUERY_ATTRIBUTES specifically; the general object-type check
  already passed in B13-B15.
- Win32k sends QueryDeviceRelations (likely TargetDeviceRelation, verify
  from PML Detail) and holds the handle open until 20:10:29.807 — same ms
  as PreDisconnect.
- The disconnect fires 234 ms after the failed IOCTL, 25 ms after
  MonitorArrival.

### What this does not establish

Two hypotheses fit the full picture:

(a) Win32k requires a functional keyboard by IDD arrival. Null and broken
    handles both fail; the IOCTL failure is the mechanism.

(b) IDD arrival kills the session regardless; the failed IOCTL is
    incidental.

The handle being held open through QueryDeviceRelations cuts against
"win32k wrote it off immediately" and is the one data point prior handoffs
skipped. It is a hypothesis-consistent observation, not proof.

B16b (mouse slot, 20 min, no rebuild) and B17 (UMDF keyboard device)
distinguish (a) from (b).

### IOCTL calibration (verified against ntddkbd.h)

| IOCTL | function | code |
|---|---|---|
| QUERY_ATTRIBUTES | 0x0000 | 0xb0000 (B16) |
| SET_TYPEMATIC | 0x0001 | 0xb0004 |
| SET_INDICATORS | 0x0002 | 0xb0008 |
| QUERY_TYPEMATIC | 0x0008 | 0xb0020 |
| QUERY_INDICATORS | 0x0010 | 0xb0040 |
| QUERY_INDICATOR_TRANSLATION | 0x0020 | 0xb0080 |
| INSERT_DATA | 0x0040 | 0xb0100 |
| QUERY_EXTENDED_ATTRIBUTES | 0x0080 | 0xb0200 |
| QUERY_IME_STATUS | 0x0400 | 0xb1000 |
| SET_IME_STATUS | 0x0401 | 0xb1004 |

### Next

B16b: mouse slot with Procmon filtered to hydra-file-input.txt. Prediction:
IOCTL_MOUSE_QUERY_ATTRIBUTES at 0xf0000 (verify against ntddmou.h).

### Tag force-update note

The hydra tag mode4-B15-2026-11-13 was created before the B13-B15 commit
landed and pointed at 826b64b instead of b746ddb. It has been force-updated
to b746ddb. Any clone that fetched the old tag will keep the old value until
it runs:

    git fetch --tags --force

Otherwise the tag points at different commits depending on who asked.

## B16b, 2026-11-13 late — mouse slot Procmon: no IOCTL, no probe

Filtered Procmon on a run with `input-file-mou.flag` only (mouse slot = File
handle, keyboard and beep null). Three rows in the trace:

```
20:28:48.035  svchost.exe  CreateFile   hydra-file-input.txt  SUCCESS
20:28:48.418  csrss.exe    CloseFile                         SUCCESS
20:28:48.418  csrss.exe    IRP_MJ_CLOSE                      SUCCESS
```

**No DeviceIoControl from csrss.exe.** Contrast B16 keyboard slot:
IOCTL_KEYBOARD_QUERY_ATTRIBUTES at +34 ms.

Provider log confirms mouse-only:
`GetInputHandles -> out kbd=0x0 mou=0x86C beep=0x0 mode=file-mou`

### What this establishes

- win32k does not issue keyboard-class or mouse-class IOCTLs against the
  mouse slot handle. The mouse slot is not in the probe path.
- Every run with a non-functional keyboard slot (null or File) and an IDD
  present died at reason=18. The mouse slot content makes no observable
  difference (B14 and B16b both had mouse=File, both died the same way).
- Keyboard is the only input class win32k actively probes in this path.

### B17 spec update

**Only a keyboard UMDF device is needed.** Mouse device not required.
`mou` and `beep` slots stay null in the first B17.

### Hypothesis (a) vs (b) still open

B16b narrows the design (keyboard only) but not the causal question.
Hypothesis (b) — IDD arrival kills the session regardless of handles —
remains consistent with every run so far. B17 (functional keyboard UMDF)
is the test.

### Timing note

CloseFile at .418, 382 ms after CreateFile. B16's equivalent was 268 ms.
Different session-establishment window — cannot compare directly without
PreDisconnect timestamps. PreDisconnect fires at .417, same ms as CloseFile.
Recorded for future reference.

## B17 — 2026-11-14 — UMDF keyboard device

### Result

**Hypothesis (b) confirmed.** A fully functional UMDF keyboard device in the
GetInputHandles keyboard slot does not prevent reason=18. The session dies
regardless. The failed QUERY_ATTRIBUTES probe that B16 observed was
incidental to the disconnect, not its cause.

Decision-table row: **row 3** — CreateFile + 0xb0000 + SUCCESS, session dies
at reason=18 within baseline window, and the device log shows no subsequent
keyboard query or read failing. Trace is clean; (b) is the reading.

### Evidence — provider.log

```
13:49:18.621 GetInputHandles   <-- the input path
13:49:18.621 GetInputHandles -> in kbd=0000000000000000 mou=0000000000000000 beep=0000000000000000
13:49:18.621 GetInputHandles: opening UMDF kbd path=\\?\root#system#0001#{a5b1c2d3-e4f5-6789-abcd-ef0123456789}
13:49:18.623 GetInputHandles: UMDF kbd opened, h=0000000000000890
13:49:18.623 GetInputHandles -> out kbd=0000000000000890 mou=0000000000000000 beep=0000000000000000 mode=umdf-kbd
13:49:18.624 GetHardwareId     *** THE STACK IS ASKING FOR THE DRIVER *** cap=200
13:49:18.624 GetHardwareId -> hr=0x00000000, len=22
13:49:18.639 ConnectNotify session=3  (IDD creation starts here)
13:49:18.717 PreDisconnect reason=18
```

Timeline (ms from GetInputHandles):
- +0    GetInputHandles called
- +2    FileCreate reached the driver (UMDF device opened)
- +3    GetHardwareId (new vs B16)
- +18   ConnectNotify — IDD creation starts
- +33   IOCTL 0xb0000 SUCCESS 28 bytes
- +34   READ pending 192 bytes
- +96   PreDisconnect reason=18

### Evidence — C:\HydraLog\keyboard-umdf.log

```
134391125586224759 FileCreate: file=FFFFFE23E94FB278
134391125586539880 IOCTL: 0x000b0000 out=28
134391125586543125   QUERY_ATTR: SUCCESS 28 bytes
134391125586549852 READ: len=192 -> pending
```

192 bytes = 16 KEYBOARD_INPUT_DATA records (12 bytes each) — a batched read.

### Evidence — UMDF operational log

```
13:49:18.706  id=2006  The UMDF Host successfully loaded the driver at level 0.
13:49:18.709  id=2101  Completed Pnp or Power operation (27, 20) with status 0x0.
```

PreDisconnect fired at 13:49:18.717, 11 ms after the last PnP operation
completed with status 0x0. The IDD stack did not fail to load.

### What this proves

- `UmdfKernelModeClientPolicy = AllowKernelModeClients` works. win32k's
  kernel-client IRP reached the UMDF driver: 0xb0000 returned SUCCESS with
  28 bytes, and the subsequent READ was accepted and left pending.
- QUERY_ATTRIBUTES SUCCESS + pending READ means win32k accepted the handle
  as a functional keyboard.
- The keyboard is not the blocker. IDD arrival kills the session regardless.

### Device implementation notes

- **Class = System, not Keyboard.** Keyboard class policy overrode the
  per-device SDDL. CreateFile returned 5 (ACCESS_DENIED) for any non-SYSTEM
  token even with a permissive SDDL. Class=System and
  ClassGUID={4d36e97d-...} fixed it.
- **SDDL needed `WD`.** `(A;;GA;;;BA)` alone does not match a filtered
  admin token. Add `(A;;GA;;;NS)(A;;GA;;;WD)`.
- **`WdfDeviceInitAssignSDDLString` is KMDF-only.** Not in any UMDF 2.x
  header. The only way to set the DACL for a UMDF driver is INF
  `[.NT.HW]` Security.
- **SDK 10.0.26100.0 + UMDF 2.25 + `/DWIN32_NO_STATUS`:** `ntstatus.h`
  does not provide `STATUS_*` constants. Provide `#ifndef`-guarded
  fallback defines.
- **`DRIVER_INITIALIZE` expands to plain `NTSTATUS`.** In a .cpp driver,
  `DriverEntry` gets C++ linkage and `WdfDriverStubUm.lib` cannot resolve
  it. Fix: `extern "C" NTSTATUS DriverEntry(...)`.
- **UMDF INF needs:** `[.NT.Wdf]` with UmdfService/UmdfServiceOrder; a
  `[Service]` section named `<service>_Install` with UmdfLibraryVersion
  and ServiceBinary; `[.NT.Services]` with `AddService=WUDFRd`.
  ServiceBinary must be `%12%\UMDF\<name>.dll` (at `%12%\` the
  co-installer returns Error 87).
- **`GetDC` collides with `HDC GetDC(HWND)`.** Prefix context accessors
  (KbdGetDC).
- **`WdfDeviceInitSetExclusive` is KMDF-only.**

### Instrument notes

- IRPMon capture was inconclusive. 69 MB captured with WUDFRd hooked
  driver-only. Per-device hook on `\Device\0080` returned
  `ERROR_NOT_FOUND` (1168) — `\0080` in DEVPKEY_Device_Driver is the PnP
  index, not the kernel device object number. After driver restart, `Ia:`
  hook took, but B17's capture contained no 0xB0000. **IRP-path proof for
  B17 comes from the driver log, not from IRPMon.**
- CLI gotchas for rc3: `--input=D:\\.\irpmndrv` (not `\\.\IRPMon`);
  `--enumerate` and `--install` are not valid options; `Ia:` works but
  requires `sc stop/start irpmndrv` if a prior `I:`-only hook exists.

### New calibration facts

- **`GetHardwareId` fires 1 ms after GetInputHandles returns non-null.**
  termsrv queries the seat hardware ID through the provider between the
  input path and ConnectNotify. `len=22` returned; whether 22 is expected
  or truncated is open for B18.
- **Session state after reason=18 with a UMDF keyboard was `ConnQ`, not
  torn down.** Session lifetime ~78 ms, sampler missed it.
- **Cleanup order matters.** `Stop-ProtoRun` logoff requires the provider
  registered. Log off the session first, verify gone, then
  `rdsprov-register.ps1 -Unregister`. If logoff fails with 1753
  (no more endpoints available), orphaned session needs
  `taskkill /f /fi "SERVICES eq TermService"` + reboot.
- **IDD stack completes load before the disconnect.** B18 should look at
  display-config activation, D3D device creation, or session-activation
  logic after IDD load — not at IDD load itself.

### Device fate

HydraKbd devnode `ROOT\SYSTEM\0001` left installed, driver running.
IRPMon `irpmndrv` service: STOPPED.


## B18 — 2026-11-14 — Skip ConfigUpdate2

### Result

**IddCxAdapterDisplayConfigUpdate2 failure is not the trigger.** With the
call skipped via `C:\HydraLog\idd-skip-config-update.flag`, `AdapterInitFinished`
returned `0x0`, and `PreDisconnect reason=18` fired 3 ms later. The 0xC000000D
from ConfigUpdate2 in B17 was incidental.

### Evidence — iddseat.log

```
14:17:15.286 D0Entry: calling IddCxAdapterInitAsync, caps.Flags=0x5
14:17:15.286 IddCxAdapterInitAsync -> 0x00000000
14:17:15.302 AdapterInitFinished: entered, AdapterInitStatus=0x00000000
14:17:15.309 ConfigUpdate2: skip flag PRESENT (err=0)
14:17:15.309 IddCxAdapterDisplayConfigUpdate2 SKIPPED
14:17:15.309 AdapterInitFinished: returning 0x00000000
```

### Evidence — provider.log

```
14:17:15.153 GetInputHandles
14:17:15.155 GetInputHandles: UMDF kbd opened, h=0x8A0
14:17:15.155 GetInputHandles -> out kbd=0x8A0 mode=umdf-kbd
14:17:15.156 GetHardwareId -> hr=0x00000000, len=22
14:17:15.166 ConnectNotify session=3
14:17:15.312 PreDisconnect reason=18
```

Same shape as B17. Handle open, hardware ID returned, ConnectNotify, disconnect.
158 ms from GetInputHandles to PreDisconnect. 3 ms from AdapterInitFinished return
to PreDisconnect.

### What this eliminates

- `IddCxAdapterDisplayConfigUpdate2 -> 0xC000000D` is not the trigger
- Keyboard path ruled out by B17
- PnP operations all complete `0x0`
- `GetHardwareId` returns correct data (`HydraSeat_RemoteIDD_v1`, 22 bytes,
  matches SWD device name)
- Wrong DLL installed ruled out: installed
  `C:\WINDOWS\System32\drivers\umdf\iddseat.dll` hash `F3D044EC...` matches
  `dist\driver-remote\iddseat.dll`. `dist\driver\iddseat.dll` hash `73580276...`
  does not match — correct remote build is installed.

### Consistent signal across B17 and B18

| Run | AdapterInitFinished | PreDisconnect | Gap |
|---|---|---|---|
| B17 | `0x0` at 18.716 | 18.717 | 1 ms |
| B18 | `0x0` at 15.309 | 15.312 | 3 ms |

Both: driver reports success, termsrv disconnects 1–3 ms later. No UMDF events
fire between those two lines. Termsrv is not waiting for a driver callback —
it's checking adapter state and rejecting.

### Open for B19 — `caps.Flags=0x5` candidate

The log shows `IddCxAdapterInitAsync, caps.Flags=0x5`. Per `iddcx.h`:

- `0x2` — IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP_UPDATE
- `0x4` — IDDCX_ADAPTER_FLAGS_USE_SMALLER_BUFFERS
- `0x10` — IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER

`0x5` = `0x1 | 0x4`. Does **not** include `0x10`. If the remote build is supposed
to set the remote-session adapter flag but doesn't, that's the likely cause:
termsrv loaded a driver identifying as a remote IDD (via GetHardwareId) but
the adapter isn't flagged remote-capable.

`build-driver.ps1`'s own comment references this exact risk: "newer headers,
older declared extension, runtime-guarded calls — but whether the framework
accepts it here is untested."

**B19's next read:** `Select-String -Path C:\Programs\hydra\iddseat\iddseat.cpp
-Pattern 'HYDRA_REMOTE_IDD|REMOTE_SESSION_DRIVER|caps.Flags|IDDCX_ADAPTER_FLAGS'
-Context 2,8` — confirm the flag is meant to be set and whether it's actually
compiling under `-Remote`.

Caveat: if `IddCxAdapterInitAsync` masks unknown flags on a driver declaring
`IddCx0102`, setting `0x10` may also be ignored — in which case B19 becomes
a version-declaration fix (`UmdfExtensions=IddCx0110` in `iddseat-remote.inf`)
rather than a code fix.

### Phantom session behavior

Same as B17. After reason=18, session 3 remained in `ConnQ`. `logoff` failed
with 1753 (no more endpoints available). `taskkill /f /fi "SERVICES eq
TermService"` followed by `Start-Service TermService` did not clear it. The
session lives in `smss.exe`'s kernel session table, not TermService's. Only
a reboot clears a ConnQ phantom.

### Device fate

HydraKbd devnode `ROOT\SYSTEM\0001` still present, driver running (unchanged
from B17). Provider unregistered after reboot. `irpmndrv` state: unloaded
at handoff (to be reconfirmed by B19 baseline).