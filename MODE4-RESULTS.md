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
