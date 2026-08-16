# fix-viewprobe.ps1 -- stop hydra-view.ps1 dying on mirror's success message.
#
# THE BUG
#   Line ~88:  $probe = & $mirror $Seat --probe 2 2>&1 | Out-String
#
#   mirror.exe writes "[mirror] pixel transport opened (Global\HydraSeat_B_pix)"
#   to STDERR. It is a SUCCESS line, not an error. The 2>&1 turns that stderr
#   record into an ErrorRecord, and with $ErrorActionPreference = 'Stop' at the
#   top of the script the whole run aborts -- right at the point where it was
#   about to confirm frames exist and start both mirrors.
#
#   This is new, not a regression in hydra-view.ps1: PowerShell 7.4 enabled
#   $PSNativeCommandUseErrorActionPreference by default, so native commands now
#   honour ErrorActionPreference. PS7 was reinstalled during the 2026-08-13
#   rebuild and is newer than the one this script was written against on 08-09.
#
#   Same family as hydra-shm.ps1's Write-Host blocking redirection to a file.
#
# THE FIX
#   Bracket that one call with ErrorActionPreference = 'Continue' and restore
#   it afterwards. Narrow by design -- the script's 'Stop' default is wanted
#   everywhere else.
#
# USAGE
#   .\fix-viewprobe.ps1
#   .\fix-viewprobe.ps1 -Revert

param(
    [string]$File = 'C:\Programs\hydra\hydra-view.ps1',
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $File)) { throw "not found: $File" }

if ($Revert) {
    $bak = Get-ChildItem "$File.bak-*" -EA SilentlyContinue |
           Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $bak) { throw "no backup matching $File.bak-*" }
    Copy-Item $bak.FullName $File -Force
    Write-Host "reverted from $($bak.Name)" -ForegroundColor Yellow
    return
}

$lines = [System.IO.File]::ReadAllLines($File)

if (($lines -join "`n") -match 'PSNativeCommandUseErrorActionPreference|HYDRA_PROBE_EAP') {
    Write-Host "already patched." -ForegroundColor Yellow
    return
}

# Find the probe line by content, not by number -- line numbers move.
$idx = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -like '*--probe*' -and $lines[$i] -like '*2>&1*') { $idx = $i; break }
}
if ($idx -lt 0) { throw "could not find the mirror --probe line. File not modified." }

Write-Host "anchor: line $($idx + 1)" -ForegroundColor DarkGray
Write-Host "  $($lines[$idx].Trim())" -ForegroundColor DarkGray

$bak = "$File.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $File $bak -Force
Write-Host "backup: $bak" -ForegroundColor DarkGray

# Preserve the original indentation so the file stays readable.
$indent = ([regex]::Match($lines[$idx], '^\s*')).Value

$before = @(
    "$indent# HYDRA_PROBE_EAP: mirror writes its 'pixel transport opened' SUCCESS line to",
    "$indent# stderr. On PS 7.4+ \$PSNativeCommandUseErrorActionPreference is ON by default,",
    "$indent# so 2>&1 under 'Stop' turns that into a terminating error and kills the run.",
    "$indent`$__eap = `$ErrorActionPreference",
    "$indent`$ErrorActionPreference = 'Continue'"
)
$after = @(
    "$indent`$ErrorActionPreference = `$__eap"
)

$out = @()
if ($idx -gt 0) { $out += $lines[0..($idx - 1)] }
$out += $before
$out += $lines[$idx]
$out += $after
if ($idx + 1 -lt $lines.Count) { $out += $lines[($idx + 1)..($lines.Count - 1)] }

[System.IO.File]::WriteAllLines($File, $out)

# Verify it still parses -- a broken launcher is worse than the bug.
$errs = $null
[System.Management.Automation.Language.Parser]::ParseFile($File, [ref]$null, [ref]$errs) | Out-Null
if ($errs -and $errs.Count) {
    Copy-Item $bak $File -Force
    $errs | ForEach-Object { Write-Host "  $($_.Message)" -ForegroundColor Red }
    throw "patched file does not parse -- reverted."
}

Write-Host ""
Write-Host "patched and parses OK:" -ForegroundColor Green
Get-Content $File | Select-Object -Skip ([Math]::Max(0, $idx - 1)) -First 9 |
    ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }

Write-Host ""
Write-Host "Test:" -ForegroundColor Cyan
Write-Host "  `$env:HYDRA_GFX='RFX'; .\hydra-view.ps1 -Desktop 2"
Write-Host ""
Write-Host "Expect: it gets PAST the probe, then starts BOTH mirrors itself." -ForegroundColor Cyan
Write-Host "Check with: Get-Process mirror | Select-Object Id, WorkingSet" -ForegroundColor Cyan
Write-Host "STATE.md says two processes, 70-98 MB each. ~7 MB = empty ring." -ForegroundColor Cyan
