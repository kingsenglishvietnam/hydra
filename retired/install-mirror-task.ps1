# install-mirror-task.ps1 -- run mirror as a LOGON TASK, not a service child.
#
# WHY THIS EXISTS
#   mirror creates a window and presents a D3D11 swapchain onto the seat's panel.
#   That needs an interactive token with foreground-activation rights. A process
#   launched by a Windows SERVICE into the console session does NOT get them.
#
#   The symptom is nasty to diagnose, because nothing errors: hydrad reports
#   "mirror:B running", the process really is alive, and its log is completely
#   EMPTY -- while the identical binary launched by hand from an interactive
#   shell works perfectly and paints the panel immediately. Same exe, same
#   session, different token.
#
#   So mirror is taken out of the service entirely. hydrad keeps the genuinely
#   background pieces (capture, input routing, audio) and mirror runs here, as a
#   scheduled task set to "run only when the user is logged on" -- which is
#   exactly the interactive context it needs. Starts at logon, no console window,
#   nothing to type each session.
#
# USAGE (elevated):
#   .\install-mirror-task.ps1                 # register for seat B on DISPLAY2
#   .\install-mirror-task.ps1 -Seat B -Monitor '\\.\DISPLAY2'
#   .\install-mirror-task.ps1 -Remove         # unregister
#
# After registering, either log off and back on, or start it now with:
#   Start-ScheduledTask -TaskName "Hydra Mirror B"

param(
    [string]$Seat    = 'B',
    [string]$Monitor = '\\.\DISPLAY2',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$taskName = "Hydra Mirror $Seat"
$exe      = Join-Path $PSScriptRoot 'dist\mirror.exe'

if ($Remove) {
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
        Write-Host "removed task '$taskName'" -ForegroundColor Yellow
    } else {
        Write-Host "no task '$taskName' to remove"
    }
    return
}

if (-not (Test-Path $exe)) { throw "mirror.exe not found at $exe -- run .\build.ps1 first" }

# The task runs as the CURRENT interactive user, only while logged on. That is
# the whole point: it inherits a real interactive token.
$user = "$env:USERDOMAIN\$env:USERNAME"

$action = New-ScheduledTaskAction -Execute $exe `
                                  -Argument "$Seat `"$Monitor`"" `
                                  -WorkingDirectory (Join-Path $PSScriptRoot 'dist')

$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user

# Highest available so it can sit above other topmost windows on the panel.
$principal = New-ScheduledTaskPrincipal -UserId $user `
                                        -LogonType Interactive `
                                        -RunLevel Highest

# Defaults that matter: don't stop it on battery, don't kill it after 3 days,
# restart it if it ever exits.
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -StartWhenAvailable

if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}

Register-ScheduledTask -TaskName $taskName `
                       -Action $action `
                       -Trigger $trigger `
                       -Principal $principal `
                       -Settings $settings | Out-Null

Write-Host "registered '$taskName'" -ForegroundColor Green
Write-Host "  runs: $exe $Seat `"$Monitor`""
Write-Host "  as:   $user (interactive, only while logged on)"
Write-Host ""
Write-Host "Start it now without logging off:" -ForegroundColor Cyan
Write-Host "  Start-ScheduledTask -TaskName `"$taskName`""
Write-Host ""
Write-Host "Check it:" -ForegroundColor Cyan
Write-Host "  Get-ScheduledTask -TaskName `"$taskName`" | Select State"
Write-Host "  Get-Process mirror -ErrorAction SilentlyContinue"
