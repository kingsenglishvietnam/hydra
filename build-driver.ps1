#requires -RunAsAdministrator
# build-driver.ps1 -- compile + link iddseat.dll (IddCx UMDF driver) command-line.
#   .\build-driver.ps1            # console IDD (default, working version)
#   .\build-driver.ps1 -Remote    # remote-session IDD (experimental test build)
param([switch]$Remote)
#
# Uses WDK 28000 headers, IddCx 1.11 stub, UMDF 2.35 stub. Run from an ELEVATED
# x64 Native Tools / VS Dev prompt -> powershell, in the hydra folder:
#   .\build-driver.ps1
#
# Produces dist\driver\iddseat.dll (+ copies the .inf). Signing is separate
# (sign-driver.ps1). This is the fiddly one; read any error block it prints.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$outdir = Join-Path $root 'dist\driver'
New-Item -ItemType Directory -Force -Path $outdir | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe not on PATH. Run from an x64 Native Tools prompt (then 'powershell')."
}

# --- locate the kit pieces (pin to what we verified is installed) ---
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer  = '10.0.28000.0'                 # headers/libs version present on this box
$iddcx   = '1.11'                          # newest IddCx headers/stub present
$umdf    = '2.35'                          # newest UMDF stub present

$inc = @(
    "$kitRoot\Include\$sdkVer\um",
    "$kitRoot\Include\$sdkVer\shared",
    "$kitRoot\Include\$sdkVer\km",                       # wudfwdm.h / driver-side
    "$kitRoot\Include\$sdkVer\um\iddcx\$iddcx",
    "$kitRoot\Include\wdf\umdf\$umdf"
)
$libs = @(
    "$kitRoot\Lib\$sdkVer\um\x64\iddcx\$iddcx\iddcxstub.lib",
    "$kitRoot\Lib\wdf\umdf\x64\$umdf\WdfDriverStubUm.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\d3d11.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\dxgi.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\avrt.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\advapi32.lib",   # InitializeSecurityDescriptor / SetSecurityDescriptorDacl
    "$kitRoot\Lib\$sdkVer\um\x64\kernel32.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\ole32.lib"
)

# The UMDF WDF stub references DbgPrintEx, which lives in the UMDF user-mode
# ntdll import lib. Its path differs from the SDK um libs -- find it under the
# UMDF lib tree. Try the known locations; add whichever exists.
$ntdllUm = @(
    "$kitRoot\Lib\wdf\umdf\x64\$umdf\ntdllUm.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\ntdll.lib"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($ntdllUm) { $libs += $ntdllUm } else { Write-Warning "no ntdllUm/ntdll lib found for DbgPrintEx" }

# sanity: include dirs must exist (hard fail); libs we resolve individually below
foreach ($d in $inc)  { if (-not (Test-Path $d)) { Write-Error "missing include dir: $d" } }

# Resolve each lib: some (advapi32, ole32, kernel32, ntdll) may live in a
# different subpath than the iddcx/umdf ones. Search the kit if the direct
# path misses, so we fail with a clear "can't find X" not a bad hardcoded path.
$resolved = @()
foreach ($l in $libs) {
    if (Test-Path $l) { $resolved += $l; continue }
    $name = Split-Path $l -Leaf
    $found = Get-ChildItem "$kitRoot\Lib\$sdkVer\um\x64" -Filter $name -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if ($found) { $resolved += $found.FullName }
    else { Write-Warning "lib not found, skipping: $name (may cause LNK2019)" }
}
$libs = $resolved
Write-Host "kit paths OK (SDK $sdkVer, IddCx $iddcx, UMDF $umdf); $($libs.Count) libs" -ForegroundColor Green

$incArgs = $inc | ForEach-Object { "/I$_" }

# UMDF driver: user-mode DLL, but compiled with driver defines. _WIN32_WINNT 0x0A00,
# UMDF_USING_NTSTATUS + WIN32_NO_STATUS avoid NTSTATUS dup-def between ntstatus & windows.
$cldefs = @(
    '/D_UNICODE','/DUNICODE',
    '/D_WIN32_WINNT=0x0A00',
    '/DUMDF_USING_NTSTATUS',
    '/DWIN32_NO_STATUS',
    '/DNTDDI_VERSION=0x0A000010'          # 24H2-ish; IddCx needs >= 1709
)
if ($Remote) {
    $cldefs += '/DHYDRA_REMOTE_IDD'
    Write-Host "*** REMOTE-SESSION IDD build (experimental) ***" -ForegroundColor Magenta
}

$src = Join-Path $root 'iddseat\iddseat.cpp'
$obj = Join-Path $outdir 'iddseat.obj'
$dll = Join-Path $outdir 'iddseat.dll'

Write-Host "compiling iddseat.cpp ..." -ForegroundColor Cyan
$compile = @('/nologo','/c','/EHsc','/std:c++17','/W3','/MD') + $cldefs + $incArgs + @($src,"/Fo:$obj")
$out = & cl.exe @compile 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "COMPILE FAILED:" -ForegroundColor Red
    $out | Where-Object { $_ -match ': (error|fatal error) ' } | Select-Object -First 20 |
           ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
    Write-Error "compile stage failed"
}
Write-Host "  compiled OK" -ForegroundColor Green

Write-Host "linking iddseat.dll ..." -ForegroundColor Cyan
# /DLL, no default libs pulled that clash with UMDF; entry is the WDF stub.
$link = @('/nologo','/DLL',"/OUT:$dll",$obj) + $libs +
        @('/NODEFAULTLIB:kernel32.lib') # UMDF stub provides its own; re-add via our explicit list
$out = & link.exe @link 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "LINK FAILED:" -ForegroundColor Red
    $out | Select-Object -First 40 |
           ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
    Write-Error "link stage failed"
}
Write-Host "  linked OK" -ForegroundColor Green

Copy-Item (Join-Path $root 'iddseat\iddseat.inf') $outdir -Force
Write-Host ""
Write-Host "Built: $dll" -ForegroundColor Green
Get-Item $dll | ForEach-Object { "  {0:N0} bytes" -f $_.Length }
Write-Host "Next: stampinf + inf2cat + .\sign-driver.ps1, then pnputil /add-driver." -ForegroundColor Cyan
