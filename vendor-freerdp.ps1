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
# TWO CLIENTS, TWO DIRECTORIES  (added 2026-08-13)
#   dist\freerdp\sdl-freerdp.exe   -- mode 2, the teaching client
#   dist\hydrardp.exe              -- mode 3, our own FreeRDP-based client
#
#   Both link libfreerdp3. sdl-freerdp gets its DLLs vendored beside it; for a
#   long time hydrardp got its DLLs from MSYS2 being on PATH during development,
#   which silently stopped being true after the 2026-08-12 reset. See
#   INCIDENT-2026-08-12.md.
#
#   Do NOT trust `ldd hydrardp.exe` here. It reports clean once the three static
#   imports (libfreerdp-client3, libfreerdp3, libwinpr3) are present, but FreeRDP
#   delay-loads its channel and codec modules at runtime and those never appear
#   in the static import table. hydrardp needs the full resolved set as siblings,
#   which is what -Hydrardp does.
#
# USAGE (elevated not required):
#   .\vendor-freerdp.ps1
#   .\vendor-freerdp.ps1 -Source C:\msys64\mingw64\bin
#   .\vendor-freerdp.ps1 -SkipHydrardp        # dist\freerdp\ only, old behaviour

param(
    [string]$Source        = 'C:\msys64\mingw64\bin',
    [string]$Exe           = 'sdl-freerdp.exe',
    [string]$Dest          = "$PSScriptRoot\dist\freerdp",
    [string]$HydrardpDir   = "$PSScriptRoot\dist",
    [switch]$SkipHydrardp
)

$ErrorActionPreference = 'Stop'

# PS 7.4 made native-command stderr honour ErrorActionPreference. Several tools
# here write PROGRESS to stderr -- hydractl's 'not reachable' while it waits,
# mirror's 'pixel transport opened' -- and 2>&1 under 'Stop' turned those
# SUCCESS lines into terminating errors. This broke hydra-start.ps1 on 2026-08-21.
$PSNativeCommandUseErrorActionPreference = $false
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
} else {
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
}

# Licence, since we are redistributing.
foreach ($lic in @("$Source\..\share\licenses\freerdp\LICENSE",
                   "$Source\..\share\licenses\freerdp\LICENSE.txt")) {
    if (Test-Path $lic) { Copy-Item $lic (Join-Path $Dest 'FreeRDP-LICENSE.txt') -Force; break }
}

# --- hydrardp's own copy ----------------------------------------------------
#
# Mirror the resolved set beside dist\hydrardp.exe. Same DLLs, second location:
# Windows resolves imports from the directory of the loading executable, and
# hydrardp.exe is in dist\, not dist\freerdp\.
#
# The whole set rather than a minimal one, deliberately. The minimal set is not
# knowable from the static import table (delay-loaded modules), and ~90 DLLs of
# duplication is cheaper than a client that fails to start on a rebuilt machine.

if (-not $SkipHydrardp) {
    $hydrardp = Join-Path $HydrardpDir 'hydrardp.exe'
    if (-not (Test-Path $hydrardp)) {
        Write-Warning "hydrardp.exe not found at $hydrardp -- skipping. Build it, then re-run."
    } else {
        $vendored = Get-ChildItem $Dest -Filter *.dll
        if ($vendored.Count -eq 0) {
            Write-Warning "no DLLs vendored into $Dest -- nothing to mirror."
        } else {
            Copy-Item $vendored.FullName $HydrardpDir -Force
            Write-Host "mirrored $($vendored.Count) DLL(s) beside hydrardp.exe" -ForegroundColor Green

            # Prove it, rather than assuming. Anything other than a clean exit or
            # usage text means an import is still unresolved.
            $probe = & $hydrardp 2>&1 | Select-Object -First 1
            if ($probe -match 'usage') {
                Write-Host "hydrardp.exe loads and prints usage -- imports resolve" -ForegroundColor Green
            } else {
                Write-Warning "hydrardp.exe did not print usage. First line was:"
                Write-Warning "  $probe"
                Write-Warning "Check with: ldd from an MSYS2 shell, or Dependencies.exe for delay-loads."
            }
        }
    }
}

Write-Host ""
Write-Host "Test it before removing MSYS2:" -ForegroundColor Cyan
Write-Host "  $Dest\$Exe /version"
Write-Host ""
Write-Host "Then point Hydra at the vendored copy:" -ForegroundColor Cyan
Write-Host "  .\hydra-start.ps1 -Client freerdp -FreeRdpPath '$Dest\$Exe'"
Write-Host ""
Write-Host "Mode 3 uses dist\hydrardp.exe directly:" -ForegroundColor Cyan
Write-Host "  .\dist\hydrardp.exe B teacher '' 127.0.0.2"
Write-Host ""
Write-Host "NOTE: dist\ is gitignored, so this is not committed. Move it outside" -ForegroundColor Yellow
Write-Host "dist\ if you want the binaries in the repo." -ForegroundColor Yellow
