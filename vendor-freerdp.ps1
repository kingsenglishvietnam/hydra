# vendor-freerdp.ps1 -- copy SDL-FreeRDP and its DLLs into the tree.
#
# WHY
#   FreeRDP is not optional any more: it is the fix for the panel freeze. Both
#   mstsc and SDL-FreeRDP were measured with a probe that generates its own
#   activity, and only FreeRDP keeps streaming while its window is minimized OR
#   fully covered. mstsc sends a Suppress Output PDU and the seat's desktop stops
#   being composed.
#
#   But depending on a whole MSYS2 development environment to supply one client
#   is a poor arrangement for a classroom machine, and MSYS2 is not in the repo,
#   so a rebuild elsewhere would not have it. This copies the client and the DLLs
#   it actually needs into dist\freerdp\, after which MSYS2 can be removed.
#
#   FreeRDP is Apache 2.0, so redistributing the binaries is fine. The licence is
#   copied alongside them.
#
# USAGE (elevated not required):
#   .\vendor-freerdp.ps1
#   .\vendor-freerdp.ps1 -Source C:\msys64\mingw64\bin

param(
    [string]$Source = 'C:\msys64\mingw64\bin',
    [string]$Exe    = 'sdl-freerdp.exe',
    [string]$Dest   = "$PSScriptRoot\dist\freerdp"
)

$ErrorActionPreference = 'Stop'

$src = Join-Path $Source $Exe
if (-not (Test-Path $src)) { throw "not found: $src" }

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item $src $Dest -Force
Write-Host "copied $Exe" -ForegroundColor Green

# Resolve dependencies with dumpbin (ships with VS Build Tools, already required
# to build Hydra). Recursive, because FreeRDP's DLLs have their own.
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    Write-Warning "dumpbin not on PATH -- run this from an x64 Native Tools prompt."
    Write-Warning "Falling back to copying every DLL in $Source (large but works)."
    Copy-Item (Join-Path $Source '*.dll') $Dest -Force
    return
}

$seen    = @{}
$pending = New-Object System.Collections.Queue
$pending.Enqueue($src)

while ($pending.Count -gt 0) {
    $file = $pending.Dequeue()
    $out  = & $dumpbin.Source /dependents $file 2>$null

    foreach ($line in $out) {
        if ($line -match '^\s{4}(\S+\.dll)\s*$') {
            $dll = $Matches[1]
            if ($seen.ContainsKey($dll)) { continue }
            $seen[$dll] = $true

            # Only vendor DLLs that live alongside the exe -- system DLLs come
            # from Windows and must NOT be copied.
            $cand = Join-Path $Source $dll
            if (Test-Path $cand) {
                Copy-Item $cand $Dest -Force
                $pending.Enqueue($cand)
            }
        }
    }
}

Write-Host "copied $($seen.Keys.Count) dependency name(s); $((Get-ChildItem $Dest -Filter *.dll).Count) DLL(s) vendored" -ForegroundColor Green

# Licence, since we are redistributing.
foreach ($lic in @("$Source\..\share\licenses\freerdp\LICENSE",
                   "$Source\..\share\licenses\freerdp\LICENSE.txt")) {
    if (Test-Path $lic) { Copy-Item $lic (Join-Path $Dest 'FreeRDP-LICENSE.txt') -Force; break }
}

Write-Host ""
Write-Host "Test it before removing MSYS2:" -ForegroundColor Cyan
Write-Host "  $Dest\$Exe /version"
Write-Host ""
Write-Host "Then point Hydra at the vendored copy:" -ForegroundColor Cyan
Write-Host "  .\hydra-start.ps1 -Client freerdp -FreeRdpPath '$Dest\$Exe'"
Write-Host ""
Write-Host "NOTE: dist\ is gitignored, so this is not committed. Move it outside" -ForegroundColor Yellow
Write-Host "dist\ if you want the binaries in the repo." -ForegroundColor Yellow
