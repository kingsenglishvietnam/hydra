# fix-milestone-banner.ps1 -- retire the stale usage banner in hydrardp.c
#
# WHY
#   dist\hydrardp.exe still prints:
#
#     MILESTONE 1: connects and counts frames. Publishes nothing yet.
#
#   That has been false since the gfx-working fixes of 2026-08-11. hydrardp
#   publishes frames to the shared-memory pixel ring from the gfx EndFrame
#   callback, and a student confirmed clean video on seat B. The string was
#   simply never updated.
#
#   Cosmetic, but a usage banner that lies is exactly the kind of thing that
#   misleads at 2am during a real fault.
#
# NOTES
#   Uses ReadAllText/WriteAllText and \r?\n-tolerant matching, because
#   hydrardp.c has mixed line endings (1048 CRLF, 44 bare LF introduced by
#   earlier patch scripts). Do NOT use Get-Content/Set-Content here -- it will
#   normalise them and produce a diff touching the whole file.
#
# USAGE:
#   .\fix-milestone-banner.ps1              # gate + patch
#   .\fix-milestone-banner.ps1 -WhatIfOnly  # gate only, changes nothing

param(
    [string]$File    = "$PSScriptRoot\rdp\hydrardp.c",
    [string]$NewText = 'publishes frames to Global\\HydraSeat_B_pix on gfx EndFrame',
    [switch]$WhatIfOnly
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $File)) { throw "not found: $File" }

# --- GATE -------------------------------------------------------------------
# Find the banner before touching anything. If this does not report exactly one
# hit, stop and look -- the string may have been split across printf calls.

$hits = Select-String -Path $File -Pattern 'MILESTONE 1'
if (-not $hits) {
    Write-Host "no 'MILESTONE 1' in $File -- already fixed, or the text moved." -ForegroundColor Yellow
    return
}

Write-Host "found $($hits.Count) hit(s):" -ForegroundColor Cyan
foreach ($h in $hits) {
    Write-Host ("  line {0}: {1}" -f $h.LineNumber, $h.Line.Trim())
}

if ($hits.Count -ne 1) {
    Write-Warning "expected exactly 1 hit. Stopping -- inspect the lines above and patch by hand."
    return
}

if ($WhatIfOnly) {
    Write-Host ""
    Write-Host "would replace the MILESTONE 1 sentence with:" -ForegroundColor Cyan
    Write-Host "  $NewText"
    Write-Host "nothing changed. re-run without -WhatIfOnly." -ForegroundColor Yellow
    return
}

# --- backup -----------------------------------------------------------------

$bak = "$File.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $File $bak
Write-Host "backup: $bak" -ForegroundColor DarkGray

# --- patch ------------------------------------------------------------------
# Replace only the sentence, not the whole line -- the line is a printf/fprintf
# with format specifiers and a newline escape we must not disturb.

$t   = [System.IO.File]::ReadAllText($File)
$old = $t

$t = $t -replace 'MILESTONE 1: connects and counts frames\.\s*Publishes nothing yet\.', $NewText

if ($t -eq $old) {
    # Sentence punctuation differs from what we expected. Fall back to replacing
    # from 'MILESTONE 1' up to the closing quote of that string literal.
    $t = $t -replace 'MILESTONE 1:[^"\\]*', $NewText
}

if ($t -eq $old) {
    Write-Warning "matched 'MILESTONE 1' but no replacement pattern fired."
    Write-Warning "Patch by hand at line $($hits[0].LineNumber). Backup is at $bak."
    return
}

[System.IO.File]::WriteAllText($File, $t)

# --- verify -----------------------------------------------------------------

$after = Select-String -Path $File -Pattern 'MILESTONE 1|HydraSeat_B_pix on gfx EndFrame'
Write-Host ""
Write-Host "after:" -ForegroundColor Cyan
foreach ($h in $after) {
    Write-Host ("  line {0}: {1}" -f $h.LineNumber, $h.Line.Trim())
}

if (Select-String -Path $File -Pattern 'MILESTONE 1' -Quiet) {
    Write-Warning "'MILESTONE 1' still present -- check the output above."
} else {
    Write-Host ""
    Write-Host "banner retired. Rebuild and confirm:" -ForegroundColor Green
    Write-Host "  .\build-rdpclient.ps1"
    Write-Host "  .\dist\hydrardp.exe"
}
