# fix-umdfversion.ps1 -- add an explicit UmdfLibraryVersion substitution step to
#                        build-driver.ps1.
#
# WHY
#   Per 1427b7a: stampinf in build-kbfilter.ps1 uses -f -d -a -v, none of which
#   touch UmdfLibraryVersion, so porting that call would not fix the token. The
#   source INFs legitimately still contain the literal $UMDFVERSION$ and
#   build-driver.ps1 copies them verbatim -- so dist\driver ships the literal
#   token on every build. dist\driver-remote works only because it was
#   hand-edited to 2.33.0 once, and that edit is lost on the next build.
#
#   This is a BUILD HYGIENE bug, not mode 4's blocker. The blocker is
#   0xD000000D -- UMDF refusing the load before DriverEntry, constant across
#   UMDF 2.35 and 2.33 (fb346cb). Do not conflate them.
#
# WHAT IT DOES
#   Inserts substitution immediately after the Copy-Item that places the INF in
#   the output directory. Substitutes in the COPY, never the source -- the
#   source keeps the token so the value can only ever come from $umdf, which is
#   the same variable the include and lib paths are built from. It therefore
#   cannot drift from what the DLL is actually linked against.
#
#   Writes with WriteAllText and no BOM. INF files are read by setupapi before
#   any code runs; a BOM on line 1 is a class of failure worth not inviting.
#
# USAGE
#   .\fix-umdfversion.ps1
#   .\fix-umdfversion.ps1 -Revert

param(
    [string]$File = 'C:\Programs\hydra\build-driver.ps1',
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

if (($lines -join "`n") -match 'UMDFVERSION') {
    Write-Host "already patched -- UMDFVERSION substitution present." -ForegroundColor Yellow
    return
}

# Line-based insertion with a content gate. The earlier regex approach failed on
# PowerShell's own $-escaping inside a .NET regex; matching on two literal
# substrings is both simpler and harder to get wrong.
$idx = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -like '*Copy-Item*' -and
        $lines[$i] -like '*iddseat*'   -and
        $lines[$i] -like '*$outdir*') { $idx = $i; break }
}
if ($idx -lt 0) {
    throw "could not find the INF Copy-Item line. File not modified."
}

Write-Host "anchor: line $($idx + 1)" -ForegroundColor DarkGray
Write-Host "  $($lines[$idx].Trim())" -ForegroundColor DarkGray

$bak = "$File.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $File $bak -Force
Write-Host "backup: $bak" -ForegroundColor DarkGray

$insert = @'

# --- substitute UmdfLibraryVersion in the COPY, never the source -------------
#
# The source INFs carry the literal $UMDFVERSION$ token deliberately, so the
# value can only ever come from $umdf above -- the same variable the include and
# lib paths are built from. It therefore cannot drift from what the DLL is
# actually linked against.
#
# stampinf does NOT do this. Its -f -d -a -v switches do not touch
# UmdfLibraryVersion, which is why porting build-kbfilter.ps1's call here would
# have changed nothing (commit 1427b7a). Handing a co-installer the literal
# string produces error 87, "the parameter is incorrect".
#
# NOTE: this fixes the token, NOT mode 4. The blocker is 0xD000000D -- UMDF
# refusing the load before DriverEntry, unchanged across UMDF 2.35 and 2.33
# (fb346cb).
#
# No BOM: setupapi reads the INF before any of our code runs.
$infOut = Join-Path $outdir ($(if ($Remote) { 'iddseat-remote.inf' } else { 'iddseat.inf' }))
$infTxt = [System.IO.File]::ReadAllText($infOut)
if ($infTxt -match [regex]::Escape('$UMDFVERSION$')) {
    $infTxt = $infTxt -replace [regex]::Escape('$UMDFVERSION$'), "$umdf.0"
    [System.IO.File]::WriteAllText($infOut, $infTxt, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "  UmdfLibraryVersion -> $umdf.0" -ForegroundColor Green
} else {
    Write-Warning "no UMDFVERSION token in $infOut -- already substituted, or the INF changed."
}

# Prove it rather than assume it.
$check = Select-String -Path $infOut -Pattern '^\s*UmdfLibraryVersion\s*=' |
         Select-Object -First 1 -ExpandProperty Line
if (-not $check)       { Write-Error "no UmdfLibraryVersion line in $infOut" }
if ($check -match '\$'){ Write-Error "UmdfLibraryVersion still holds a literal token: $check" }
Write-Host "  $($check.Trim())" -ForegroundColor DarkGray
'@ -split "`r?`n"

$out = @()
$out += $lines[0..$idx]
$out += $insert
if ($idx + 1 -lt $lines.Count) { $out += $lines[($idx + 1)..($lines.Count - 1)] }

[System.IO.File]::WriteAllLines($File, $out)

$verify = Select-String -Path $File -Pattern 'UMDFVERSION|UmdfLibraryVersion' |
          Select-Object LineNumber, Line
if ($verify.Count -lt 3) {
    Copy-Item $bak $File -Force
    throw "verification failed ($($verify.Count) hits, expected 3+) -- reverted."
}

Write-Host ""
Write-Host "patched:" -ForegroundColor Green
$verify | ForEach-Object { Write-Host ("  {0,5}  {1}" -f $_.LineNumber, $_.Line.Trim()) }

Write-Host ""
Write-Host "Test it -- build only, nothing is staged or installed:" -ForegroundColor Cyan
Write-Host "  .\build-driver.ps1"
Write-Host "  .\build-driver.ps1 -Remote"
Write-Host ""
Write-Host "Then confirm both outputs carry a real version:" -ForegroundColor Cyan
Write-Host "  Select-String -Path dist\driver\iddseat.inf, dist\driver-remote\iddseat-remote.inf -Pattern 'UmdfLibraryVersion'"
Write-Host ""
Write-Host "Reminder: this fixes the token, NOT mode 4. The blocker is 0xD000000D" -ForegroundColor Yellow
Write-Host "-- UMDF refusing the load before DriverEntry, unchanged across 2.35 and" -ForegroundColor Yellow
Write-Host "2.33. Next instrument is the WUDF framework trace (see 6d79e0b)." -ForegroundColor Yellow
