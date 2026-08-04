# audio-pin.ps1 -- make the RDP client's audio output assignment survive reboots.
#
# THE PROBLEM
#   Seat isolation depends on ONE setting: mstsc.exe's per-app output device is
#   the monitor, while the system default stays on the laptop speakers. That
#   setting has been silently reset by every reboot, and by removing an audio
#   device -- costing a puzzled minute each morning, because the UI can go on
#   showing the right device while the stored entry points at nothing.
#
#   Windows keeps these under
#     HKCU\Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore
#   as subkeys with hashed names whose default value encodes
#     <endpoint-id>|<exe-path>%b{role-guid}
#   There is no supported API to set them, but they can be captured and restored
#   verbatim -- which is all persistence needs.
#
# USAGE:
#   .\audio-pin.ps1 -Save     # capture the CURRENT working assignment
#   .\audio-pin.ps1 -Apply    # put it back (safe to run every startup)
#   .\audio-pin.ps1 -Show     # print what's stored and what's live
#
# Run -Save ONCE while the audio is working correctly. After that -Apply restores
# that exact state. Apply BEFORE launching mstsc: a change won't take on an audio
# stream that is already open.

param(
    [switch]$Save,
    [switch]$Apply,
    [switch]$Show,
    [string]$App = 'mstsc.exe',
    [string]$StateFile = "$PSScriptRoot\audio-pin.json"
)

$ErrorActionPreference = 'Stop'
$store = 'HKCU:\Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore'

function Get-AppEntries {
    if (-not (Test-Path $store)) { return @() }
    Get-ChildItem $store -ErrorAction SilentlyContinue | ForEach-Object {
        $v = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).'(default)'
        if ($v -and $v -like "*$App*") {
            [pscustomobject]@{ Key = [string]$_.PSChildName; Value = [string]$v }
        }
    }
}

if ($Show) {
    Write-Host "live entries for ${App}:" -ForegroundColor Cyan
    $live = @(Get-AppEntries)
    if ($live) { $live | ForEach-Object { Write-Host "  $($_.Key)" ; Write-Host "    $($_.Value)" } }
    else { Write-Host "  (none)" -ForegroundColor Yellow }

    if (Test-Path $StateFile) {
        Write-Host "`nsaved in $StateFile :" -ForegroundColor Cyan
        (Get-Content $StateFile -Raw | ConvertFrom-Json) | ForEach-Object {
            Write-Host "  $($_.Key)"; Write-Host "    $($_.Value)"
        }
    } else {
        Write-Host "`nno saved state -- run:  .\audio-pin.ps1 -Save" -ForegroundColor Yellow
    }
    return
}

if ($Save) {
    $live = @(Get-AppEntries)
    if (-not $live) {
        Write-Warning "no entries found for $App. Play audio in teacher's session, set"
        Write-Warning "Volume mixer -> $App -> the monitor, then run -Save again."
        return
    }
    $live | ConvertTo-Json -Depth 4 | Set-Content $StateFile -Encoding UTF8
    Write-Host "saved $($live.Count) entry/entries for $App to $StateFile" -ForegroundColor Green
    $live | ForEach-Object { Write-Host "  $($_.Value)" -ForegroundColor DarkGray }
    return
}

if ($Apply) {
    if (-not (Test-Path $StateFile)) {
        Write-Host "no saved audio assignment ($StateFile) -- skipping" -ForegroundColor DarkGray
        return
    }
    $saved = @(Get-Content $StateFile -Raw | ConvertFrom-Json)
    if (-not (Test-Path $store)) { New-Item -Path $store -Force | Out-Null }

    # Drop any CURRENT entries for this app first. Windows accumulates one per
    # change and never removes the old ones, so a stale entry pointing at a
    # device that no longer exists can win over the correct one -- which is
    # exactly the "it looks right but plays nowhere" state.
    $removed = 0
    foreach ($e in @(Get-AppEntries)) {
        Remove-Item (Join-Path $store ([string]$e.Key)) -Recurse -Force -ErrorAction SilentlyContinue
        $removed++
    }

    foreach ($e in $saved) {
        $kp = Join-Path $store ([string]$e.Key)
        if (-not (Test-Path $kp)) { New-Item -Path $kp -Force | Out-Null }
        Set-ItemProperty -Path $kp -Name '(default)' -Value $e.Value
    }
    Write-Host "audio assignment restored for $App ($($saved.Count) entry/entries, $removed stale removed)" -ForegroundColor Green
    return
}

Write-Host "usage: .\audio-pin.ps1 -Save | -Apply | -Show"
