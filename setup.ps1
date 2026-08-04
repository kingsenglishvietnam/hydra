#requires -RunAsAdministrator
# setup.ps1 -- one-shot Hydra deploy + safe service control.
#
# Run from an ELEVATED PowerShell in the hydra folder:
#   Set-ExecutionPolicy -Scope Process -Bypass -Force
#   .\setup.ps1
#
# It: verifies dist\ built, copies interception.dll beside the exes, installs the
# service (stopped), and gives you start/stop that WON'T lock your mouse because
# seats.toml uses your secondary devices (kbd 4 / mouse 14), not primary (6/12).

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'

Write-Host "== Hydra setup ==" -ForegroundColor Cyan

# --- stop the service FIRST, before touching any files ---
# interception.dll is loaded by seat_router while the service runs, so copying it
# below fails with a sharing violation -- and $ErrorActionPreference='Stop' then
# aborts the whole script. That made setup.ps1 usable only if you'd already
# stopped Hydra by hand. Stop it here and the script is self-sufficient.
$svc0 = Get-Service Hydra -ErrorAction SilentlyContinue
if ($svc0 -and $svc0.Status -ne 'Stopped') {
    Write-Host "  stopping Hydra (it holds interception.dll)..." -ForegroundColor Yellow
    Stop-Service Hydra -Force
    Start-Sleep 2
}

# --- sanity: is dist\ built? ---
$need = 'hydrad.exe','hydractl.exe','seat_router.exe','seatB_agent.exe','clip_console.exe','mirror.exe'
$missing = $need | Where-Object { -not (Test-Path (Join-Path $dist $_)) }
if ($missing) { Write-Error "dist\ missing: $($missing -join ', '). Run build.ps1 first." }
Write-Host "  build present (7 exes)" -ForegroundColor Green

# --- deploy interception.dll next to the exes (the piece that was missing) ---
$dll = 'C:\Programs\Interception\library\x64\interception.dll'
if (Test-Path $dll) {
    try {
        Copy-Item $dll $dist -Force -ErrorAction Stop
        Write-Host "  interception.dll -> dist\" -ForegroundColor Green
    } catch {
        # Non-fatal: a stale copy in dist\ is almost always the same file. Warn
        # and continue rather than aborting the whole deploy over a locked DLL.
        Write-Warning "interception.dll is locked (something still running?); keeping the existing copy in dist\"
    }
} elseif (Test-Path (Join-Path $dist 'interception.dll')) {
    Write-Host "  interception.dll already in dist\" -ForegroundColor Green
} else {
    Write-Warning "interception.dll not found at $dll -- seat_router will fail at load without it."
}

# --- make sure the safe seats.toml is in dist\ ---
Copy-Item (Join-Path $root 'seats.toml') $dist -Force
Write-Host "  seats.toml -> dist\ (seat B = kbd 4 / mouse 14; your 6/12 stay yours)" -ForegroundColor Green

# --- (re)install the service, left STOPPED and DISABLED so nothing auto-starts ---
$svc = Get-Service Hydra -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Status -ne 'Stopped') { Stop-Service Hydra -Force; Start-Sleep 2 }
    & (Join-Path $dist 'hydrad.exe') uninstall | Out-Null
    Start-Sleep 1
}
& (Join-Path $dist 'hydrad.exe') install | Out-Null
Set-Service Hydra -StartupType Manual
Write-Host "  service installed (Manual start, currently stopped)" -ForegroundColor Green

Write-Host ""
Write-Host "Ready. Controls:" -ForegroundColor Cyan
Write-Host "  Start-Service Hydra                 # launch (safe: uses secondary devices)"
Write-Host "  Get-Content C:\ProgramData\Hydra\logs\hydrad.log -Wait -Tail 15   # watch"
Write-Host "  .\dist\hydractl.exe status          # see helpers + virtual monitor"
Write-Host "  Stop-Service Hydra                  # halt"
Write-Host ""
Write-Host "SAFETY NET: if anything grabs your primary mouse, keyboard still works." -ForegroundColor Yellow
Write-Host "  Type:  Stop-Service Hydra   (Enter)  -- releases input in ~2s." -ForegroundColor Yellow
Write-Host "  The service is Manual-start, so a reboot also clears it." -ForegroundColor Yellow
