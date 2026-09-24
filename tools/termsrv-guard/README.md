# termsrv-guard

Keeps Hydra's seats working across Windows updates.

Hydra relies on Windows allowing more than one interactive session at a time.
Client editions of Windows (Home, Pro) allow only one, enforced inside
`C:\Windows\System32\termsrv.dll`. The usual workaround is to patch a few bytes
in that DLL. Cumulative updates regularly replace the DLL, which silently undoes
the patch: seats connect, the desktop appears, and about ten seconds later the
session is logged off (`ERRINFO_LOGOFF_BY_USER` in the FreeRDP log).

`hydra-termsrv-guard.ps1` runs at every boot, notices when the DLL has been
replaced, and re-applies the patch before anyone logs in. If Microsoft has
changed the code so that no known pattern matches, it leaves the DLL alone and
says so loudly rather than guessing.

## Quick start

From an elevated PowerShell:

```powershell
Set-Location C:\Programs\hydra\tools\termsrv-guard
Unblock-File .\hydra-termsrv-guard.ps1
.\hydra-termsrv-guard.ps1 -DryRun     # report what it would do
.\hydra-termsrv-guard.ps1             # patch now (restarts TermService, no reboot needed)
```

Then test two seats logging in at once. If both stay up and `qwinsta` lists
two `rdp-tcp#` sessions, register the boot task (see below).

## Switches

| Switch       | Effect |
|--------------|--------|
| *(none)*     | Check the DLL and patch it if needed |
| `-DryRun`    | Report only; changes nothing |
| `-Force`     | Patch even if RDP sessions are active (they will drop when TermService restarts) |
| `-Restore`   | Put back the archived original for the current build |
| `-Install`   | Register the boot-time scheduled task (see note below) |
| `-Uninstall` | Remove the scheduled task |
| `-Root <path>` | Where `termsrv-archive\`, `state\` and `logs\` live. Defaults to the repo root (two levels up, where `hydra7.ps1` is), else `C:\Programs\hydra` |

## Boot task

The task runs the script as SYSTEM at startup, under the in-box Windows
PowerShell 5.1, which has a fixed path:

```powershell
.\hydra-termsrv-guard.ps1 -Install
```

or, by hand:

```powershell
$action = New-ScheduledTaskAction `
    -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
    -Argument '-NoProfile -ExecutionPolicy Bypass -File "C:\Programs\hydra\tools\termsrv-guard\hydra-termsrv-guard.ps1"'
$trigger = New-ScheduledTaskTrigger -AtStartup
$princ   = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
Register-ScheduledTask -TaskName 'Hydra termsrv guard' `
    -Action $action `
    -Trigger $trigger `
    -Principal $princ `
    -Force
```

> Versions before 2026-09-24 had `-Install` prefer `pwsh.exe`. A Store-installed
> PowerShell 7 has its version number in its path, so the task broke on the next
> Store update. Fixed; if you registered with an old copy, run `-Install` again.

Test it without rebooting:

```powershell
Start-ScheduledTask 'Hydra termsrv guard'
Start-Sleep 5
Get-Content C:\Programs\hydra\logs\termsrv-guard.log -Tail 3
```

## Prerequisite warnings

Every run also checks two things the patch can't fix, and logs a `WARN` line
without changing either:

- **Remote Desktop switched off** (`fDenyTSConnections = 1`): TermService won't
  start and seats fail with error 10061.
- **`ServiceDll` not pointing at `termsrv.dll`**: usually RDP Wrapper is still
  installed (see below).

## How it decides

1. Hash `termsrv.dll`. If the hash equals the one it produced last time, exit: already patched.
2. Search the DLL for each pattern in the table.
   - A pattern matching **more than once** is treated as ambiguous: abort, change nothing.
   - The first pattern matching **exactly once** is chosen.
3. If no pattern matches but a known *replacement* sequence is present, the DLL
   was patched by something else: record that and exit.
4. If nothing matches at all: unknown build. Log an ERROR, create
   `state\termsrv-UNPATCHED`, change nothing.
5. Otherwise: archive the original, take ownership, stop TermService, write the
   patched bytes, return ownership to TrustedInstaller, restore the original
   ACL, restart TermService, and verify both the new hash and that the service
   is running.

## Files

All paths are currently fixed under `C:\Programs\hydra`.

| Path | Contents | In git? |
|------|----------|---------|
| `termsrv-archive\termsrv_<version>_<hash8>.dll` | Every original DLL the guard has seen; never overwritten | **No** (Microsoft binary) |
| `state\termsrv-guard.json` | Version, original and patched hashes, pattern used | No |
| `state\termsrv-UNPATCHED` | Present only when the last run could not patch; contains the build number | No |
| `logs\termsrv-guard.log` | One line per event | No |

Launchers can check for `state\termsrv-UNPATCHED` before starting seats and
fail with a clear message instead of the ten-second logoff.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Patched, already patched, or task (un)installed |
| 1 | `-Restore` refused: current DLL is not the one this script patched |
| 2 | Unknown build or ambiguous pattern; DLL untouched |
| 3 | Pattern table error (find/replace length mismatch) |
| 4 | RDP sessions active; re-run with `-Force` |
| 5 | Written DLL's hash doesn't match what was intended |
| 6 | TermService didn't come back up; consider `-Restore` |

## Pattern table

`??` is a wildcard byte. Newest first.

| Name | Find | Replace |
|------|------|---------|
| `26100.8521+` | `8B 8F 38 06 00 00 45 3B C1 75` | `8B 8F 38 06 00 00 45 3B C1 EB` |
| `24H2` | `8B 81 38 06 00 00 39 81 3C 06 00 00 75` | `B8 00 01 00 00 89 81 38 06 00 00 90 EB` |
| `legacy` | `39 81 3C 06 00 00 0F 84 ?? ?? ?? ??` | `B8 00 01 00 00 89 81 38 06 00 00 90` |

Confirmed working on termsrv.dll 10.0.26100.9549 with `26100.8521+`.

Patterns come from community sources, chiefly
[fabianosrc/TermsrvPatcher](https://github.com/fabianosrc/TermsrvPatcher),
which is the first place to look when a new build turns up.

### Adding a pattern

When the log reports `UNKNOWN BUILD`:

1. Find a search/replace pair for that build.
2. Add it to the **top** of `$Patterns` in the script. Find and Replace must be the same length.
3. Run with `-DryRun` and confirm the new pattern shows `find-hits=1` and the others show `0`.
4. Run without `-DryRun`, then test two seats.
5. Commit the updated table, noting the build it was confirmed on.

## RDP Wrapper: don't run both

Hydra used RDP Wrapper until 2026-09-24 (see `retired/RDPWRAP.md`). The wrapper
patches `termsrv.dll` in memory at offsets listed per exact build in
`rdpwrap.ini`; this guard patches the file on disk by code pattern. Both target
the same check, so running both stacks one patch on top of the other: untested,
and silent.

Why the switch:

- **Offsets break on every build; patterns usually don't.** Build 9549 shipped
  before the community ini had offsets for it, and seat B stopped working. The
  guard's `26100.8521+` pattern matched 9549 unchanged.
- **No Defender exclusion.** The wrapper's install folder had to be permanently
  excluded from scanning.

What was given up: the wrapper never modified a system file, so its uninstall
was perfectly clean. The guard's undo depends on its own archive and `-Restore`,
and `sfc /scannow` reverts the patch until the next boot.

Check that the wrapper is gone:

```powershell
Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters `
    -Name ServiceDll
```

This should show `%SystemRoot%\System32\termsrv.dll`. If it shows `rdpwrap.dll`,
uninstall the wrapper (`RDPWInst.exe -u`). At the very least, never update its
ini while the guard's patch is in place.

**The uninstall switches Remote Desktop off.** `RDPWInst.exe -u` sets
`fDenyTSConnections` to `1`, TermService stops starting, and seats fail with
`ERRCONNECT_CONNECT_FAILED` (error 10061, nothing listening on 3389). Turn it
back on:

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' `
    -Name fDenyTSConnections `
    -Value 0
Start-Service TermService
```

Enabling it this way leaves the Remote Desktop firewall rules disabled, which is
what Hydra wants: loopback works, and the rest of the network can't reach 3389.

## Things that undo the patch

Besides Windows Update: `sfc /scannow`, DISM repairs and in-place upgrades all
restore the stock DLL. The guard catches these at the next boot.

## Licensing

The Windows client licence does not permit concurrent remote sessions; patching
the DLL doesn't change that. This repository contains only byte patterns, never
Microsoft's DLL. Use on your own machines, at your own discretion.
