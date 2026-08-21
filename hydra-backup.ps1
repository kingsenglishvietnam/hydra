# hydra-backup.ps1 -- refresh the emergency recovery stick.
#
# WHY THIS EXISTS
#   2026-08-12: a driver install produced a boot loop that cost an OS reset. The
#   logs that would have explained it were destroyed by the reset -- setupapi.dev.log,
#   CBS.log and System.evtx are NOT in Windows.old.
#   2026-08-20: a driver install produced a blank screen on boot. Recovered by
#   unplugging a USB hub. That was luck, not process.
#
#   Everything this project needs to be rebuilt from a bare machine lives in
#   places a reset destroys. This puts it somewhere a reset cannot reach.
#
# WHAT IT CAPTURES
#   - the whole git history as a single verifiable bundle
#   - dist\ binaries, including the signed driver packages
#   - C:\Programs\rdsprov and C:\Programs\vdd, neither of which is in git
#   - the docs, readable from WinPE
#   - machine-specific registry state that a reset silently reverts and that
#     costs an afternoon to rediscover
#
# USAGE (elevated)
#   .\hydra-backup.ps1
#   .\hydra-backup.ps1 -Drive F:
#   .\hydra-backup.ps1 -SkipBinaries      # docs, bundle and registry only, fast

param(
    [string]$Drive = '',
    [string]$Root  = 'C:\Programs\hydra',
    [switch]$SkipBinaries
)

$ErrorActionPreference = 'Continue'
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

# ------------------------------------------------------------ find the stick --
if (-not $Drive) {
    $v = Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.DriveLetter } |
         Sort-Object SizeRemaining -Descending | Select-Object -First 1
    if (-not $v) { Say "No removable drive found. Plug in the recovery stick, or pass -Drive." Red; return }
    $Drive = "$($v.DriveLetter):"
    Say "using $Drive  ('$($v.FileSystemLabel)', $([math]::Round($v.SizeRemaining/1GB,1)) GB free)" Green
}

if (-not (Test-Path $Drive)) { Say "$Drive not found." Red; return }

# Is it still bootable? The whole point of level 3 is a stick you can boot from.
$bootable = (Test-Path "$Drive\EFI") -or (Test-Path "$Drive\sources\boot.wim") -or (Test-Path "$Drive\bootmgr")
if ($bootable) { Say "bootable recovery media present -- good" Green }
else { Say "WARNING: no boot files on $Drive. This is a data copy, NOT recovery media." Yellow }

$dest = "$Drive\hydra-backup"
New-Item -ItemType Directory -Force $dest | Out-Null
Set-Location $Root

# --------------------------------------------------------------- git bundle --
# One file containing every commit, branch and tag. Clone from it with
# `git clone hydra-repo.bundle`. Better than copying .git: single file, and
# `git bundle verify` proves it is intact.
Say ""
Say "git bundle ..." Cyan
git bundle create "$dest\hydra-repo.bundle" --all 2>&1 | Out-Null
$v = git bundle verify "$dest\hydra-repo.bundle" 2>&1 | Out-String
if ($v -match 'is okay') { Say "  bundle verified" Green }
else { Say "  BUNDLE DID NOT VERIFY -- do not trust it" Red; Write-Host $v -ForegroundColor DarkRed }

# Uncommitted work would be lost, so say so plainly.
$dirty = git status --short 2>&1 | Out-String
if ($dirty.Trim()) {
    Say "  UNCOMMITTED CHANGES -- these are NOT in the bundle:" Yellow
    Write-Host $dirty -ForegroundColor DarkYellow
    Copy-Item "$Root\*.ps1", "$Root\*.md", "$Root\seats.toml", "$Root\*.rdp" "$dest\loose\" -Force -EA SilentlyContinue
    New-Item -ItemType Directory -Force "$dest\loose" | Out-Null
    Copy-Item "$Root\*.ps1", "$Root\*.md", "$Root\seats.toml", "$Root\*.rdp" "$dest\loose\" -Force -EA SilentlyContinue
    Say "  loose copies of scripts/docs/config -> $dest\loose" Yellow
}

# --------------------------------------------------------------- docs ---------
# Also at the root of the stick, not just under hydra-backup, so they are
# findable from a WinPE command prompt without knowing the layout.
Say ""
Say "docs ..." Cyan
Copy-Item "$Root\*.md" $dest -Force -EA SilentlyContinue
foreach ($d in 'HANDOFF.md','REBUILD.md','MODES.md','MODE6-START.md','INCIDENT-2026-08-12.md','FUTURE.md') {
    if (Test-Path "$Root\$d") { Copy-Item "$Root\$d" "$Drive\" -Force }
}
Say "  key docs also at the root of $Drive (readable from WinPE)" Green

# --------------------------------------------------------- registry state -----
# Machine-specific values a reset silently reverts. Each of these cost real time
# to rediscover after 2026-08-12.
Say ""
Say "registry state ..." Cyan
$reg = "$dest\registry"
New-Item -ItemType Directory -Force $reg | Out-Null

$exports = @{
    'termservice-params'  = 'HKLM\SYSTEM\CurrentControlSet\Services\TermService\Parameters'
    'terminal-server'     = 'HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server'
    'class-keyboard'      = 'HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96B-E325-11CE-BFC1-08002BE10318}'
    'class-mouse'         = 'HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96F-E325-11CE-BFC1-08002BE10318}'
    'class-display'       = 'HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    'wudf'                = 'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF'
    'driversearching'     = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching'
    'policies-system'     = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
    'audio-policyconfig'  = 'HKCU\Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore'
}
foreach ($k in $exports.Keys) {
    & reg.exe export $exports[$k] "$reg\$k.reg" /y *> $null
    if (Test-Path "$reg\$k.reg") { Say "  $k" DarkGray }
}

# The settings that are one line each but take an afternoon to rediscover.
@"
Hydra machine state -- captured $(Get-Date -Format 'yyyy-MM-dd HH:mm')

TermService type        : $((& sc.exe qc TermService | Select-String 'TYPE').ToString().Trim())
ServiceDll              : $((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' ServiceDll -EA SilentlyContinue).ServiceDll)
fDenyTSConnections      : $((Get-ItemProperty 'HKLM:\System\CurrentControlSet\Control\Terminal Server' fDenyTSConnections -EA SilentlyContinue).fDenyTSConnections)
testsigning             : $((bcdedit /enum "{current}" | Select-String 'testsigning').ToString().Trim())
PromptOnSecureDesktop   : $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' PromptOnSecureDesktop -EA SilentlyContinue).PromptOnSecureDesktop)
SearchOrderConfig       : $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching' SearchOrderConfig -EA SilentlyContinue).SearchOrderConfig)

Interception UpperFilters
  keyboard : $((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E96B-E325-11CE-BFC1-08002BE10318}' UpperFilters -EA SilentlyContinue).UpperFilters -join ',')
  mouse    : $((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E96F-E325-11CE-BFC1-08002BE10318}' UpperFilters -EA SilentlyContinue).UpperFilters -join ',')

Audio endpoints (seats.toml must match one of these)
$(& "$Root\dist\route_endpoint.exe" --list 2>&1 | Out-String)

Displays (clip_console is DPI-aware; Windows.Forms is NOT and lies)
$(& "$Root\dist\clip_console.exe" 2>&1 | Out-String)

Driver packages staged
$((pnputil /enum-drivers | Out-String -Width 300) -split "`r?`n" | Where-Object { $_ -match 'iddseat|mttvdd|iddsample' } | Out-String)

Virtual display device
$(Get-PnpDevice -EA SilentlyContinue | Where-Object FriendlyName -match 'Virtual Display|Hydra Virtual' | Select-Object Status, InstanceId | Out-String)
"@ | Set-Content "$dest\MACHINE-STATE.txt" -Encoding UTF8
Say "  MACHINE-STATE.txt written" Green

# --------------------------------------------------------------- binaries -----
if (-not $SkipBinaries) {
    Say ""
    Say "binaries ..." Cyan

    # dist\ is gitignored -- ~271 MB, and it is what makes the repo runnable
    # without a full toolchain rebuild.
    robocopy "$Root\dist" "$dest\dist" /E /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Null
    Say "  dist\  ($([math]::Round((Get-ChildItem "$dest\dist" -Recurse -File | Measure-Object Length -Sum).Sum/1MB)) MB)" Green

    # NOT in git and NOT reproducible without re-downloading and rebuilding.
    foreach ($p in 'C:\Programs\rdsprov','C:\Programs\vdd') {
        if (Test-Path $p) {
            $n = Split-Path $p -Leaf
            robocopy $p "$dest\$n" /E /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Null
            Say "  $n\" Green
        }
    }

    # The signed driver packages, so a rebuild is not needed to reinstall.
    foreach ($p in 'dist\driver','dist\driver-remote','dist\vdd','dist\mssample') {
        if (Test-Path "$Root\$p") { Say "    (included in dist: $p)" DarkGray }
    }
} else {
    Say ""
    Say "binaries skipped (-SkipBinaries)" Yellow
}

# ------------------------------------------------------------ safety snaps ----
if (Test-Path "$Root\safety") {
    robocopy "$Root\safety" "$dest\safety" /E /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Null
    Say "  safety\ snapshots (undo-online.ps1, driver inventories, class filters)" Green
}

# ---------------------------------------------------------------- verify ------
Say ""
Say "=== result ===" Cyan
Get-ChildItem $dest | Select-Object Name, @{n='MB';e={ if ($_.PSIsContainer) { [math]::Round((Get-ChildItem $_.FullName -Recurse -File -EA SilentlyContinue | Measure-Object Length -Sum).Sum/1MB,1) } else { [math]::Round($_.Length/1MB,2) } }} | Format-Table -AutoSize
$vol = Get-Volume -DriveLetter $Drive.TrimEnd(':')
Say ("$Drive : {0:N1} GB free of {1:N1} GB" -f ($vol.SizeRemaining/1GB), ($vol.Size/1GB)) DarkGray

Say ""
Say "To restore on a bare machine:" Cyan
Say "  git clone $Drive\hydra-backup\hydra-repo.bundle C:\Programs\hydra" DarkGray
Say "  copy dist\, rdsprov\, vdd\ back into place" DarkGray
Say "  follow REBUILD.md, then MACHINE-STATE.txt for the settings a reset reverts" DarkGray
Say ""
if (-not $bootable) {
    Say "REMINDER: $Drive is not bootable. Make recovery media before the next" Yellow
    Say "driver install -- level 3 of safety-gate.ps1 depends on it." Yellow
}
