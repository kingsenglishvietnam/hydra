# install-shortcut.ps1 -- desktop shortcut: elevated x64 Native Tools + pwsh,
# landing in C:\Programs\hydra.
#
# Saves the daily ritual of: Start menu -> x64 Native Tools -> right-click ->
# Run as administrator -> pwsh -> cd C:\Programs\hydra.
#
# Elevation is set on the shortcut itself, so it prompts once on launch rather
# than needing a right-click every time.
#
# USAGE:  .\install-shortcut.ps1
#         .\install-shortcut.ps1 -Remove

param(
    [string]$Name    = 'Hydra Shell',
    [string]$WorkDir = 'C:\Programs\hydra',
    [ValidateSet('Both','Desktop','StartMenu')]
    [string]$Where   = 'Both',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
# Start menu entries live in the user's Programs folder -- no elevation needed
# to write there, unlike the all-users location.
$targets = @()
if ($Where -in @('Both','Desktop'))   {
    $targets += Join-Path ([Environment]::GetFolderPath('Desktop')) "$Name.lnk"
}
if ($Where -in @('Both','StartMenu')) {
    $progs = Join-Path ([Environment]::GetFolderPath('Programs')) 'Hydra'
    if (-not (Test-Path $progs)) { New-Item -ItemType Directory -Path $progs -Force | Out-Null }
    $targets += Join-Path $progs "$Name.lnk"
}

if ($Remove) {
    foreach ($t in $targets) {
        if (Test-Path $t) { Remove-Item $t -Force; Write-Host "removed $t" -ForegroundColor Yellow }
    }
    $progs = Join-Path ([Environment]::GetFolderPath('Programs')) 'Hydra'
    if ((Test-Path $progs) -and -not (Get-ChildItem $progs -Force)) { Remove-Item $progs -Force }
    return
}

# Find vcvars64.bat -- the edition and year vary between machines, so look
# rather than hardcode. VS 18 first: it is newer than 2022 where both exist.
$vcvars = $null
foreach ($base in @(
    "${env:ProgramFiles}\Microsoft Visual Studio",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio"))
{
    if (-not (Test-Path $base)) { continue }
    $found = Get-ChildItem $base -Recurse -Filter 'vcvars64.bat' -ErrorAction SilentlyContinue |
             Sort-Object FullName -Descending | Select-Object -First 1
    if ($found) { $vcvars = $found.FullName; break }
}
if (-not $vcvars) { throw "vcvars64.bat not found -- is Visual Studio Build Tools installed?" }
Write-Host "using $vcvars" -ForegroundColor DarkGray

# pwsh if present, else Windows PowerShell.
$shell = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
if (-not $shell) { $shell = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" }
Write-Host "using $shell" -ForegroundColor DarkGray

if (-not (Test-Path $WorkDir)) { throw "no such directory: $WorkDir" }

# cmd /k so the environment vcvars64 sets survives into the shell it launches.
$args = "/k `"`"$vcvars`" && cd /d `"$WorkDir`" && `"$shell`" -NoExit -Command `"Set-ExecutionPolicy Bypass -Scope Process -Force`"`""

$sh = New-Object -ComObject WScript.Shell
foreach ($lnk in $targets) {
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath       = "$env:SystemRoot\System32\cmd.exe"
    $s.Arguments        = $args
    $s.WorkingDirectory = $WorkDir
    $s.IconLocation     = "$env:SystemRoot\System32\imageres.dll,109"
    $s.Description      = "Elevated x64 Native Tools + PowerShell in $WorkDir"
    $s.Save()

    # The "run as administrator" bit: byte 21 of the shortcut header. There is no
    # WScript.Shell property for it, so it has to be set on the file directly.
    $bytes = [IO.File]::ReadAllBytes($lnk)
    $bytes[21] = $bytes[21] -bor 0x20
    [IO.File]::WriteAllBytes($lnk, $bytes)

    Write-Host "created: $lnk" -ForegroundColor Green
}

Write-Host ""
Write-Host "Launches elevated, with the build environment loaded, in $WorkDir." -ForegroundColor Cyan
Write-Host "Execution policy is set to Bypass for the session, so scripts just run." -ForegroundColor Cyan
Write-Host ""
Write-Host "Then simply:  .\hydra-view.ps1" -ForegroundColor Cyan
