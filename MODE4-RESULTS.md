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
