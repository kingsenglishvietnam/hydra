# build-kbfilter.ps1 -- build + sign the Hydra keyboard upper filter.
#
# PHASE 1: the driver attaches, logs each keyboard's hardware ID, and PASSES
# EVERY KEYSTROKE THROUGH. Nothing is blocked. The point of phase 1 is to prove
# the build, signing, install and attach path on a machine you can still type on.
#
# WHY THIS EXISTS AT ALL
#   Interception is dual-licensed and free only for NON-COMMERCIAL use. This
#   driver does the one thing Hydra actually needs from it -- swallow input from
#   a chosen device before the console session sees it -- so the project stops
#   depending on a third party's commercial terms.
#
# ===========================================================================
# READ THIS BEFORE INSTALLING
# ===========================================================================
# A keyboard filter sits in a boot-critical path. Have a way back:
#   * a SECOND keyboard on hand (USB), so a mistake doesn't leave you mute
#   * know how to reach Safe Mode (Settings > Recovery > Advanced startup, or
#     hold Shift while clicking Restart) -- filters are skipped there
#   * this INF is DEMAND_START with ERROR_NORMAL, so a driver that fails to load
#     is ignored rather than blocking boot
#   * removal: pnputil /delete-driver oemNN.inf /uninstall, then reboot
#
# Requires: WDK (same install used for iddseat), elevated prompt, test-signing on.

param(
    [switch]$SkipSign,
    [string]$KitRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$SdkVer  = '10.0.28000.0'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src  = Join-Path $root 'hydrakbd'
$out  = Join-Path $root 'dist\kbfilter'

Write-Host "== Hydra keyboard filter (PHASE 1: passthrough only) ==" -ForegroundColor Cyan

# --- locate the KMDF headers/libs ------------------------------------------
$incKm  = Join-Path $KitRoot "Include\$SdkVer\km"
$incSh  = Join-Path $KitRoot "Include\$SdkVer\shared"
$libKm  = Join-Path $KitRoot "Lib\$SdkVer\km\x64"

foreach ($p in @($incKm, $incSh, $libKm)) {
    if (-not (Test-Path $p)) { throw "missing WDK path: $p  (adjust -SdkVer)" }
}

# KMDF version present on this box -- pick the highest.
$kmdfInc = Get-ChildItem (Join-Path $KitRoot 'Include\wdf\kmdf') -Directory -ErrorAction SilentlyContinue |
           Sort-Object Name -Descending | Select-Object -First 1
if (-not $kmdfInc) { throw "no KMDF headers under $KitRoot\Include\wdf\kmdf" }
$kmdfVer = $kmdfInc.Name
$kmdfLib = Join-Path $KitRoot "Lib\wdf\kmdf\x64\$kmdfVer"
if (-not (Test-Path $kmdfLib)) { throw "no KMDF libs at $kmdfLib" }

Write-Host "  KMDF $kmdfVer" -ForegroundColor DarkGray

New-Item -ItemType Directory -Force -Path $out | Out-Null

# --- compile ----------------------------------------------------------------
# /kernel: no CRT, no exceptions. GS- because the kernel supplies its own
# security cookie handling. These flags are what makes it a driver rather than
# an executable that happens to include ntddk.h.
$cl = @(
    '/nologo','/c','/W4','/WX-','/O2','/Oy-','/GS-','/Gz','/kernel',
    '/D_AMD64_','/DAMD64','/D_WIN64','/DNDEBUG','/DKMDF_VERSION_MAJOR=1',
    "/I`"$incKm`"", "/I`"$incSh`"", "/I`"$($kmdfInc.FullName)`"",
    "/Fo`"$out\hydrakbd.obj`"",
    "`"$src\hydrakbd.c`""
)
Write-Host "cl  hydrakbd\hydrakbd.c"
$p = Start-Process cl.exe -ArgumentList $cl -NoNewWindow -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "compile failed ($($p.ExitCode)). Is this an x64 Native Tools prompt?" }

# --- link -------------------------------------------------------------------
# DRIVER subsystem, GsDriverEntry is KMDF's real entry point (it initialises the
# stack cookie then calls our DriverEntry).
$link = @(
    '/NOLOGO','/DRIVER','/SUBSYSTEM:NATIVE','/ENTRY:GsDriverEntry',
    '/NODEFAULTLIB','/INCREMENTAL:NO','/RELEASE','/MACHINE:X64',
    "/LIBPATH:`"$libKm`"", "/LIBPATH:`"$kmdfLib`"",
    "/OUT:`"$out\hydrakbd.sys`"",
    "`"$out\hydrakbd.obj`"",
    'ntoskrnl.lib','hal.lib','wmilib.lib','BufferOverflowFastFailK.lib',
    'WdfLdr.lib','WdfDriverEntry.lib'
)
Write-Host "link hydrakbd.sys"
$p = Start-Process link.exe -ArgumentList $link -NoNewWindow -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "link failed ($($p.ExitCode))" }

Copy-Item (Join-Path $src 'hydrakbd.inf') $out -Force

# --- catalog + sign ---------------------------------------------------------
if (-not $SkipSign) {
    $stampinf = Join-Path $KitRoot "bin\$SdkVer\x64\stampinf.exe"
    if (Test-Path $stampinf) {
        & $stampinf -f (Join-Path $out 'hydrakbd.inf') -d '*' -a 'amd64' -v '*'
    }

    $inf2cat = Get-ChildItem (Join-Path $KitRoot 'bin') -Recurse -Filter inf2cat.exe -ErrorAction SilentlyContinue |
               Select-Object -First 1
    if ($inf2cat) {
        & $inf2cat.FullName "/driver:$out" /os:10_X64
    } else {
        Write-Warning "inf2cat not found -- catalog not generated"
    }

    # Reuse the HydraTest certificate already trusted for iddseat.
    $signer = Join-Path $root 'sign-driver.ps1'
    if (Test-Path $signer) { & $signer -DriverDir $out }
    else { Write-Warning "sign-driver.ps1 missing -- driver is UNSIGNED and will not load" }
}

Write-Host ""
Write-Host "Built: $out\hydrakbd.sys" -ForegroundColor Green
Write-Host ""
Write-Host "INSTALL -- have a second keyboard to hand first:" -ForegroundColor Yellow
Write-Host "  pnputil /add-driver `"$out\hydrakbd.inf`" /install"
Write-Host "  (then reboot -- filters attach when the stack is rebuilt)"
Write-Host ""
Write-Host "VERIFY (phase 1 is passthrough; typing must behave exactly as now):" -ForegroundColor Cyan
Write-Host "  run DebugView from Sysinternals, with Capture Kernel enabled"
Write-Host "  expect:  [hydrakbd] attached to keyboard: HID\VID_..."
Write-Host "           [hydrakbd] hooked service callback for ..."
Write-Host "           [hydrakbd] ...: N keys seen (passthrough)"
Write-Host ""
Write-Host "REMOVE:" -ForegroundColor Yellow
Write-Host "  pnputil /enum-drivers | Select-String hydrakbd"
Write-Host "  pnputil /delete-driver oemNN.inf /uninstall     (then reboot)"
