# undo-online.ps1 -- generated 20260813-194953 for 'iddseat-hostsharing'
# Reverses the driver package(s) added since the snapshot. Run ELEVATED.
$ErrorActionPreference = 'Stop'
$snap = 'C:\Programs\hydra\safety\iddseat-hostsharing-20260813-194953'
dism /online /get-drivers /format:table > "$snap\drivers-after.txt"
$before = (Select-String -Path "$snap\drivers-before.txt" -Pattern 'oem\d+\.inf' -AllMatches).Matches.Value | Sort-Object -Unique
$after  = (Select-String -Path "$snap\drivers-after.txt"  -Pattern 'oem\d+\.inf' -AllMatches).Matches.Value | Sort-Object -Unique
$new    = $after | Where-Object { $_ -notin $before }
if (-not $new) { Write-Host 'no new driver packages; nothing to remove' -ForegroundColor Yellow }
foreach ($inf in $new) {
    Write-Host "removing $inf" -ForegroundColor Yellow
    pnputil /delete-driver $inf /uninstall /force
}
# Restore class filters to their captured values.
$f = Get-Content "$snap\class-filters.json" | ConvertFrom-Json
foreach ($g in $f.PSObject.Properties.Name) {
    $val = $f.$g
    if ($val -eq '(none)') {
        Remove-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$g" -Name UpperFilters -EA SilentlyContinue
    } else {
        $arr = $val -split '\\0'
        Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$g" -Name UpperFilters -Value $arr -Type MultiString
    }
    Write-Host "restored UpperFilters on $g" -ForegroundColor Green
}
Write-Host 'reboot to apply.' -ForegroundColor Cyan
