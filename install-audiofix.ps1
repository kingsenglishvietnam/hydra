# install-audiofix.ps1 -- one-click "Fix Audio" on the seat user's desktop.
#
# WHAT IT DOES
#   Drops a shortcut that runs:  hydractl.exe chime <seat>
#
#   That plays one short sound in the seat's session. The endpoint goes bad when
#   idle and the FIRST application to open it gets silence -- so making a
#   throwaway sound the first opener means whatever the user launches next is
#   second, and works. Verified by hand: any sound before a browser fixes the
#   browser.
#
#   hydractl only writes a line to hydrad's named pipe; hydrad does the
#   privileged work. So the shortcut needs NO elevation and produces NO UAC
#   prompt, even though the seat user is a standard account.
#
#   NOT audiofix: restarting Audiosrv was tried from four different contexts and
#   fixes nothing, because a restart leaves the endpoint IDLE and just hands the
#   problem to whoever opens it next.
#
#
# USAGE (from your console session):
#   .\install-audiofix.ps1
#   .\install-audiofix.ps1 -Seat B -TeacherUser teacher
#   .\install-audiofix.ps1 -Remove

param(
    [string]$Seat        = 'B',
    [string]$TeacherUser = 'teacher',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$desktop = "C:\Users\$TeacherUser\Desktop"
$lnk     = Join-Path $desktop 'Fix Audio.lnk'
$ctl     = Join-Path $PSScriptRoot 'dist\hydractl.exe'

# Clean up the old scheduled-task approach if it's still registered.
if (Get-ScheduledTask -TaskName 'HydraAudioFix' -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName 'HydraAudioFix' -Confirm:$false
    Write-Host "removed the old HydraAudioFix scheduled task (ran in session 0; ineffective)" -ForegroundColor Yellow
}

if ($Remove) {
    if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Host "removed $lnk" -ForegroundColor Yellow }
    else { Write-Host "no shortcut at $lnk" }
    return
}

if (-not (Test-Path $ctl))     { throw "hydractl.exe not found at $ctl -- run .\build.ps1 first" }
if (-not (Test-Path $desktop)) { throw "no desktop at $desktop -- has $TeacherUser logged in at least once?" }

$sh = New-Object -ComObject WScript.Shell
$s  = $sh.CreateShortcut($lnk)
$s.TargetPath       = $ctl
$s.Arguments        = "chime $Seat"
$s.WorkingDirectory = (Join-Path $PSScriptRoot 'dist')
$s.WindowStyle      = 7
$s.IconLocation     = "$env:SystemRoot\System32\mmres.dll,0"
$s.Description      = "Play a priming sound if audio is missing in this seat"
$s.Save()

Write-Host "created: $lnk" -ForegroundColor Green
Write-Host "  runs: $ctl chime $Seat"
Write-Host ""
Write-Host "Teacher: click 'Fix Audio', then start playback." -ForegroundColor Cyan
Write-Host "No admin prompt -- hydractl just messages the service." -ForegroundColor Cyan
Write-Host ""
Write-Host "Nothing else is disturbed -- it just plays a sound. Seat 1 is unaffected." -ForegroundColor Cyan
Write-Host ""
Write-Host "If audio keeps going quiet mid-lesson, the chime isn't holding. Set" -ForegroundColor Yellow
Write-Host "  audio_prime = `"keepalive`"   in seats.toml, then .\setup.ps1 and restart." -ForegroundColor Yellow
