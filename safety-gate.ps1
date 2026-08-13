# safety-gate.ps1 -- three-level undo verification + pre-flight capture.
#
# Run this BEFORE any boot-risk operation: pnputil /add-driver, class UpperFilters
# edits, CI/WDAC, BCD, RDP-Wrapper, ASTER, IDD staging, protocol provider
# registration.
#
# WHY THIS EXISTS
#   INCIDENT-2026-08-12: a driver stage produced a boot loop. Recovery cost an
#   OS reinstall, because:
#     - no online undo had been written before the change (nothing to run)
#     - `bcdedit /set bootstatuspolicy ignoreallfailures` was set on an already
#       failing machine, which removed the automatic failover INTO WinRE
#     - the recovery stick from the June ASTER work had never been rebuilt
#   All three levels were gone at once. That is the failure this gate prevents.
#
# THE THREE LEVELS
#   UNDO 1  cwd      -- reverse it from the running system. Snapshot + generated
#                       undo-online.ps1 that diffs the driver store and removes
#                       whatever the risky move added.
#   UNDO 2  WinRE    -- reverse it from Windows recovery when it won't boot.
#                       Requires WinRE ENABLED and bootstatuspolicy NOT set to
#                       ignoreallfailures. Generates UNDO-OFFLINE.txt with the
#                       real values filled in.
#   UNDO 3  USB      -- reverse it when WinRE is unreachable. Requires a TESTED
#                       bootable recovery stick, physically present, with the
#                       undo notes copied onto it (FAT32, readable from WinPE).
#
#   Level N is only a real undo if levels N+1 exist. A machine with only level 1
#   has no undo at all -- level 1 is exactly what a boot failure takes away.
#
# USAGE
#   .\safety-gate.ps1                      # verify + capture, refuse if not ready
#   .\safety-gate.ps1 -Label "iddseat-mode4"
#   .\safety-gate.ps1 -SkipUsb             # levels 1+2 only. NOT for driver work.
#
# EXIT: throws on any failed gate. If it returns, you are clear to proceed.

param(
    [string]$Label   = 'risky-op',
    [string]$Root    = 'C:\Programs\hydra',
    [switch]$SkipUsb
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "must run elevated"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$snap  = Join-Path $Root "safety\$Label-$stamp"
New-Item -ItemType Directory -Force -Path $snap | Out-Null

$fail = @()
$warn = @()

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

Say ""
Say "=== safety gate: $Label ===" Cyan
Say "snapshot: $snap" DarkGray
Say ""

# =========================================================================
# LEVEL 1 -- cwd / online undo
# =========================================================================
Say "[1] online undo (cwd)" Cyan

# Driver store, so the post-install diff can name what was added.
dism /online /get-drivers /format:table > (Join-Path $snap 'drivers-before.txt')
$drvCount = (Select-String -Path (Join-Path $snap 'drivers-before.txt') -Pattern 'oem\d+\.inf').Count
Say "    driver store captured ($drvCount packages)" Green

# Class filters. These are what a bad filter driver leaves behind, and what
# has to be restored by hand from WinRE if it goes wrong.
$kbdGuid = '{4D36E96B-E325-11CE-BFC1-08002BE10318}'
$mouGuid = '{4D36E96F-E325-11CE-BFC1-08002BE10318}'
$filters = [ordered]@{}
foreach ($g in @($kbdGuid, $mouGuid)) {
    $p = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$g"
    $v = (Get-ItemProperty $p -Name UpperFilters -EA SilentlyContinue).UpperFilters
    $filters[$g] = if ($v) { ($v -join '\0') } else { '(none)' }
}
$filters | ConvertTo-Json | Set-Content (Join-Path $snap 'class-filters.json')
Say "    class filters:" Green
$filters.GetEnumerator() | ForEach-Object { Say "      $($_.Key) = $($_.Value)" DarkGray }

# Services we might need to disable offline.
Get-Service Hydra, TermService, keyboard, mouse -EA SilentlyContinue |
    Select-Object Name, Status, StartType |
    ConvertTo-Json | Set-Content (Join-Path $snap 'services.json')

# Boot config, testsigning, Secure Boot.
bcdedit /enum "{current}" > (Join-Path $snap 'bcd-current.txt')
$bcd = Get-Content (Join-Path $snap 'bcd-current.txt') -Raw

# The generated online undo. Diffs the driver store and removes the delta.
@"
# undo-online.ps1 -- generated $stamp for '$Label'
# Reverses the driver package(s) added since the snapshot. Run ELEVATED.
`$ErrorActionPreference = 'Stop'
`$snap = '$snap'
dism /online /get-drivers /format:table > "`$snap\drivers-after.txt"
`$before = (Select-String -Path "`$snap\drivers-before.txt" -Pattern 'oem\d+\.inf' -AllMatches).Matches.Value | Sort-Object -Unique
`$after  = (Select-String -Path "`$snap\drivers-after.txt"  -Pattern 'oem\d+\.inf' -AllMatches).Matches.Value | Sort-Object -Unique
`$new    = `$after | Where-Object { `$_ -notin `$before }
if (-not `$new) { Write-Host 'no new driver packages; nothing to remove' -ForegroundColor Yellow }
foreach (`$inf in `$new) {
    Write-Host "removing `$inf" -ForegroundColor Yellow
    pnputil /delete-driver `$inf /uninstall /force
}
# Restore class filters to their captured values.
`$f = Get-Content "`$snap\class-filters.json" | ConvertFrom-Json
foreach (`$g in `$f.PSObject.Properties.Name) {
    `$val = `$f.`$g
    if (`$val -eq '(none)') {
        Remove-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\`$g" -Name UpperFilters -EA SilentlyContinue
    } else {
        `$arr = `$val -split '\\0'
        Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\`$g" -Name UpperFilters -Value `$arr -Type MultiString
    }
    Write-Host "restored UpperFilters on `$g" -ForegroundColor Green
}
Write-Host 'reboot to apply.' -ForegroundColor Cyan
"@ | Set-Content (Join-Path $snap 'undo-online.ps1')
Say "    undo-online.ps1 generated" Green

# =========================================================================
# LEVEL 2 -- WinRE
# =========================================================================
Say ""
Say "[2] WinRE" Cyan

$re = reagentc /info 2>&1 | Out-String
$re | Set-Content (Join-Path $snap 'reagentc.txt')

if ($re -match 'Windows RE status:\s*Enabled') {
    Say "    WinRE enabled" Green
} else {
    $fail += "WinRE is NOT enabled. Fix: reagentc /enable"
    Say "    WinRE NOT ENABLED" Red
}

# The 2026-08-12 mistake. This setting suppresses automatic failover INTO WinRE.
if ($bcd -match 'bootstatuspolicy\s+ignoreallfailures') {
    $fail += "bootstatuspolicy is ignoreallfailures -- this REMOVES automatic entry to WinRE. Fix: bcdedit /set {current} bootstatuspolicy displayallfailures"
    Say "    bootstatuspolicy = ignoreallfailures  <-- LEVEL 2 IS GONE" Red
} else {
    Say "    bootstatuspolicy ok (failover to WinRE intact)" Green
}

if ($bcd -match 'safeboot') {
    $warn += "safeboot is set in BCD -- clear it before testing a normal boot"
    Say "    safeboot set in BCD" Yellow
}

# =========================================================================
# LEVEL 3 -- USB
# =========================================================================
Say ""
Say "[3] recovery stick" Cyan

$stick = $null
if ($SkipUsb) {
    $warn += "USB gate skipped (-SkipUsb). Do NOT use this for driver installs."
    Say "    SKIPPED by request" Yellow
} else {
    $removable = Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.DriveLetter }
    foreach ($v in $removable) {
        $d = "$($v.DriveLetter):"
        if ((Test-Path "$d\bootmgr") -or (Test-Path "$d\sources\boot.wim") -or (Test-Path "$d\EFI\Boot")) {
            $stick = $d
            break
        }
    }
    if ($stick) {
        Say "    bootable media found at $stick  (label '$((Get-Volume -DriveLetter $stick[0]).FileSystemLabel)')" Green
        if ((Get-Volume -DriveLetter $stick[0]).FileSystem -ne 'FAT32') {
            $warn += "stick is not FAT32 -- Secure Boot may refuse it"
        }
    } else {
        $fail += "no bootable recovery media present. Make one: recoverydrive.exe with 'Back up system files' UNTICKED (~700MB), FAT32."
        Say "    NO BOOTABLE MEDIA PRESENT" Red
    }
}

# =========================================================================
# Offline undo sheet -- real values, readable from WinRE with `type`
# =========================================================================
$offline = @"
UNDO-OFFLINE.txt   generated $stamp   for: $Label
=====================================================================
Read this from a WinRE command prompt:   type X:\UNDO-OFFLINE.txt
=====================================================================

CRITICAL: in WinRE, HKLM is WinPE's OWN registry (a RAM hive). reg add and
reg delete against HKLM do NOTHING to the installed system. You must load the
offline hive first. Drive letters are reassigned on every WinRE boot.

--- 0. find the Windows volume -------------------------------------
for %d in (C D E F G) do @if exist %d:\Windows\System32\ntoskrnl.exe echo %d:

  Assume it says D: below. Verify before every command.

--- 1. read what failed --------------------------------------------
notepad D:\Windows\System32\LogFiles\Srt\SrtTrail.txt

  Look for "Root cause found" near the BOTTOM. If it says
  "Start network for cloud remediation 0x4c6" that is ERROR_NO_NETWORK --
  Startup Repair failing to phone home, NOT a diagnosis. Ignore it.

  dir D:\Windows\Minidump
    files dated at the failure = a driver bugchecked (dump names it)
    empty = not crashing; look elsewhere

--- 2. remove the driver package -----------------------------------
dism /image:D:\ /get-drivers /format:table > D:\drivers.txt
notepad D:\drivers.txt

  (the console scrollback is too short; notepad exists in WinRE and has Ctrl+F)
  Or filter:  dism /image:D:\ /get-drivers /format:table | find /i "iddseat"

dism /image:D:\ /remove-driver /driver:oemNN.inf

  Verify:  dism /image:D:\ /get-drivers /format:table | find /i "oemNN"
  (silence = gone)

--- 3. clear half-committed servicing -------------------------------
dism /image:D:\ /cleanup-image /revertpendingactions

  0x800f082f usually means "nothing to revert" or "not in a revertible state".
  It does NOT undo /remove-driver -- those are independent.

  If it fails and pending.xml exists:
    dir D:\Windows\WinSxS\pending.xml
    ren D:\Windows\WinSxS\pending.xml pending.old

--- 4. restore class filters ---------------------------------------
reg load HKLM\OFFSYS D:\Windows\System32\config\SYSTEM

  CHECK WHICH CONTROL SET IS LIVE FIRST:
    reg query "HKLM\OFFSYS\Select"
    Current 0x1 = ControlSet001, 0x2 = ControlSet002. Use the right one.

  KNOWN-GOOD VALUES AS OF $stamp :
    $kbdGuid  UpperFilters = $($filters[$kbdGuid])
    $mouGuid  UpperFilters = $($filters[$mouGuid])

  (a value shown as  keyboard\0kbdclass  is TWO entries -- \0 is the separator)

  Stock, with Interception REMOVED:
    reg add "HKLM\OFFSYS\ControlSet001\Control\Class\$kbdGuid" /v UpperFilters /t REG_MULTI_SZ /d "kbdclass" /f
    reg add "HKLM\OFFSYS\ControlSet001\Control\Class\$mouGuid" /v UpperFilters /t REG_MULTI_SZ /d "mouclass" /f

--- 5. disable the Hydra service offline ---------------------------
reg add "HKLM\OFFSYS\ControlSet001\Services\Hydra" /v Start /t REG_DWORD /d 4 /f

reg unload HKLM\OFFSYS

--- 6. if WinRE itself is unreachable ------------------------------
Boot the stick: shut down, hold VOLUME DOWN, press and release POWER,
keep holding Volume Down until spinning dots appear. Nothing else plugged in.

  VOLUME UP + Power = UEFI settings (NOT boot media). Easy to confuse.

Then undo the BCD settings that removed your way in:
  mountvol S: /s
  bcdedit /store S:\EFI\Microsoft\Boot\BCD /deletevalue {default} safeboot
  bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} bootstatuspolicy displayallfailures

--- 7. before any reset --------------------------------------------
COPY THESE OFF FIRST -- reset destroys them and they are NOT in Windows.old:
  xcopy D:\Windows\INF\setupapi.dev.log E:\evidence\ /Y
  xcopy D:\Windows\Logs\CBS\CBS.log E:\evidence\ /Y
  xcopy D:\Windows\System32\winevt\Logs\System.evtx E:\evidence\ /Y

  Then: Troubleshoot -> Reset this PC -> Keep my files -> LOCAL reinstall.
  NOT "Recover from a drive" (that is the full wipe).

=====================================================================
snapshot dir on the installed system: $snap
=====================================================================
"@

$offlinePath = Join-Path $snap 'UNDO-OFFLINE.txt'
$offline | Set-Content $offlinePath -Encoding ASCII
Say ""
Say "    UNDO-OFFLINE.txt generated" Green

if ($stick) {
    Copy-Item $offlinePath "$stick\UNDO-OFFLINE.txt" -Force
    Copy-Item (Join-Path $snap 'class-filters.json') "$stick\class-filters.json" -Force
    Copy-Item (Join-Path $snap 'drivers-before.txt') "$stick\drivers-before.txt" -Force
    Say "    copied to $stick (readable from WinPE)" Green
}

# =========================================================================
# Verdict
# =========================================================================
Say ""
foreach ($w in $warn) { Say "WARN: $w" Yellow }

if ($fail.Count) {
    Say ""
    Say "=== GATE FAILED -- DO NOT PROCEED ===" Red
    foreach ($f in $fail) { Say "  * $f" Red }
    Say ""
    throw "safety gate failed ($($fail.Count) blocking condition(s))"
}

$hour = (Get-Date).Hour
if ($hour -ge 22 -or $hour -lt 6) {
    Say ""
    Say "It is $((Get-Date).ToString('HH:mm')). 'Not tired' is a gate condition too." Yellow
    Say "The 2026-08-12 incident started late. Consider tomorrow." Yellow
}

Say ""
Say "=== ALL THREE LEVELS PRESENT -- clear to proceed ===" Green
Say ""
Say "  undo 1:  $snap\undo-online.ps1" DarkGray
Say "  undo 2:  WinRE enabled, failover intact" DarkGray
Say "  undo 3:  $(if ($stick) { "$stick (notes copied)" } else { 'SKIPPED' })" DarkGray
Say ""
Say "After the risky move, record what changed:" Cyan
Say "  dism /online /get-drivers /format:table > `"$snap\drivers-after.txt`""
Say ""
