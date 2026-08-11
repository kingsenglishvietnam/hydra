#requires -Version 5.1
<#
    fix-pointerbisect.ps1

    Removes the HYDRA_GFX guard around hydra_register_pointer().

    That guard was bisect scaffolding: "gdi_graphics_pipeline_init installs its
    own graphics module and may not tolerate a replaced pointer." It was one of
    eleven hypotheses for the /gfx crash, and it was wrong -- the crash was
    update->DesktopResize being NULL (found 2026-08-11, fixed separately).

    With the real cause fixed, the guard only serves to disable the cursor
    whenever gfx is on, which is PROBLEM 3 territory. Drop it.

    Run from anywhere. Verifies the exact three-line block before writing.
    -Revert restores the newest backup.
#>
[CmdletBinding()]
param(
    [string] $Source = 'C:\Programs\hydra\rdp\hydrardp.c',
    [switch] $Build,
    [switch] $Revert
)

$ErrorActionPreference = 'Stop'

if ($Revert) {
    $bak = Get-ChildItem "$Source.bak-*" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $bak) { throw "no backup found next to $Source" }
    Copy-Item $bak.FullName $Source -Force
    Write-Host "restored $($bak.Name)" -ForegroundColor Green
    return
}

if (-not (Test-Path $Source)) { throw "not found: $Source" }

$t = [System.IO.File]::ReadAllText($Source)

if ($t -notmatch 'pointer registration SKIPPED') {
    Write-Host "guard already removed -- nothing to do." -ForegroundColor Yellow
    return
}

# Match the whole block by its distinctive markers rather than by exact
# whitespace, which is what makes memory-written patches fail to apply.
$pattern = '(?s)\{ char gv\[8\].*?pointer registration SKIPPED \(gfx bisect\)"\); \}'

$m = [regex]::Matches($t, $pattern)
Write-Host "block matches found: $($m.Count)" -ForegroundColor Cyan
if ($m.Count -ne 1) {
    throw "expected exactly one match. Source has drifted -- read it before patching."
}

Write-Host ""
Write-Host "replacing:" -ForegroundColor DarkGray
Write-Host $m[0].Value -ForegroundColor DarkGray
Write-Host ""

$replacement = @'
/* The gfx guard that used to sit here was bisect scaffolding for the /gfx
     * crash. That crash was update->DesktopResize being NULL, fixed 2026-08-11,
     * so the pointer no longer needs disabling when gfx is on. */
    hydra_register_pointer(instance->context);
'@

$t = [regex]::Replace($t, $pattern, { $replacement })

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'hydra_register_pointer|pointer registration' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    & 'C:\Programs\hydra\build-rdpclient.ps1'
    Write-Host ""
    Write-Host "test:  cd C:\Programs\hydra; `$env:HYDRA_GFX='RFX'; .\dist\hydrardp.exe B teacher" -ForegroundColor Cyan
    Write-Host "expect 'pointer IMAGE received' and no 'SKIPPED' line." -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-pointerbisect.ps1 -Revert" -ForegroundColor DarkGray
}
