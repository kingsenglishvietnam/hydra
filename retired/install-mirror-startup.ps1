# install-mirror-startup.ps1 -- launch mirror from the Startup folder.
#
# WHY NOT A SERVICE, WHY NOT A SCHEDULED TASK
#   mirror creates a window and presents a D3D11 swapchain onto the seat's panel.
#   That needs a genuinely interactive token with foreground-activation rights.
#
#     - As a SERVICE child: process runs, log is EMPTY, nothing on the panel.
#     - As a SCHEDULED TASK (interactive, highest): starts, but does not reach a
#       presenting state after a reboot -- observed sitting at ~1.8 MB working
#       set burning CPU instead of the ~98 MB it uses when healthy.
#     - Run BY HAND from the user's shell: works instantly, every time.
#
#   The Startup folder is the same context as running it by hand -- Explorer
#   launches it with the logged-on user's own interactive token. So instead of
#   approximating that context, use it.
#
# USAGE (no elevation needed -- that's the point):
#   .\install-mirror-startup.ps1
#   .\install-mirror-startup.ps1 -Seat B -Monitor '\\.\DISPLAY2'
#   .\install-mirror-startup.ps1 -Remove
#
# The shortcut starts minimized so no console window appears.

param(
    [string]$Seat    = 'B',
    [string]$Monitor = '\\.\DISPLAY2',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$startup  = [Environment]::GetFolderPath('Startup')
$lnkPath  = Join-Path $startup "Hydra Mirror $Seat.lnk"
$exe      = Join-Path $PSScriptRoot 'dist\mirror.exe'

if ($Remove) {
    if (Test-Path $lnkPath) {
        Remove-Item $lnkPath -Force
        Write-Host "removed $lnkPath" -ForegroundColor Yellow
    } else {
        Write-Host "no shortcut at $lnkPath"
    }
    return
}

if (-not (Test-Path $exe)) { throw "mirror.exe not found at $exe -- run .\build.ps1 first" }

$sh  = New-Object -ComObject WScript.Shell
$lnk = $sh.CreateShortcut($lnkPath)
$lnk.TargetPath       = $exe
$lnk.Arguments        = "$Seat `"$Monitor`""
$lnk.WorkingDirectory = (Join-Path $PSScriptRoot 'dist')
$lnk.WindowStyle      = 7          # minimized -- no console in your face
$lnk.Description      = "Hydra seat $Seat panel mirror"
$lnk.Save()

Write-Host "created $lnkPath" -ForegroundColor Green
Write-Host "  target: $exe $Seat `"$Monitor`""
Write-Host ""
Write-Host "If the scheduled task is still registered, remove it or you will get TWO" -ForegroundColor Yellow
Write-Host "mirrors fighting over the panel:" -ForegroundColor Yellow
Write-Host "  .\install-mirror-task.ps1 -Remove"
Write-Host ""
Write-Host "Start it now without logging out:" -ForegroundColor Cyan
Write-Host "  Start-Process '$exe' -ArgumentList '$Seat','$Monitor' -WindowStyle Minimized"
Write-Host ""
Write-Host "Healthy mirror = ~70-98 MB working set, CPU under a second." -ForegroundColor Cyan
Write-Host "Stuck mirror   = ~2 MB and CPU climbing." -ForegroundColor Cyan
