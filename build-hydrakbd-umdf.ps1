#requires -RunAsAdministrator
# build-hydrakbd-umdf.ps1 -- compile + link HydraKbdUmdf.dll (UMDF keyboard device)
#
# Cloned from build-driver.ps1 (iddseat). UMDF 2.25 / SDK 10.0.26100.0 -- the
# same values iddseat uses. Do not bump without bumping iddseat too; the OS
# runtime on this box is 2.33 and requesting anything higher makes WUDFHost
# refuse the driver before DriverEntry.
#
# Output: dist\hydrakbd-umdf\HydraKbdUmdf.dll, plus the .inf with
# $UMDFVERSION$ substituted. Signing is separate (sign-driver.ps1).

$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$outdir = Join-Path $root 'dist\hydrakbd-umdf'
New-Item -ItemType Directory -Force -Path $outdir | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe not on PATH. Run from an x64 Native Tools prompt (then 'powershell')."
}

$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer  = '10.0.26100.0'
$umdf    = '2.25'

$inc = @(
    "$kitRoot\Include\$sdkVer\um",
    "$kitRoot\Include\$sdkVer\shared",
    "$kitRoot\Include\$sdkVer\km",
    "$kitRoot\Include\wdf\umdf\$umdf"
)
$libs = @(
    "$kitRoot\Lib\wdf\umdf\x64\$umdf\WdfDriverStubUm.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\advapi32.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\mincore.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\ole32.lib"
)

$ntdllUm = @(
    "$kitRoot\Lib\wdf\umdf\x64\$umdf\ntdllUm.lib",
    "$kitRoot\Lib\$sdkVer\um\x64\ntdll.lib"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($ntdllUm) { $libs += $ntdllUm } else { Write-Warning "no ntdllUm/ntdll lib found" }

foreach ($d in $inc) { if (-not (Test-Path $d)) { Write-Error "missing include dir: $d" } }

$resolved = @()
foreach ($l in $libs) {
    if (Test-Path $l) { $resolved += $l; continue }
    $name  = Split-Path $l -Leaf
    $found = Get-ChildItem "$kitRoot\Lib\$sdkVer\um\x64" -Filter $name -EA SilentlyContinue |
             Select-Object -First 1
    if ($found) { $resolved += $found.FullName } else { Write-Warning "lib not found: $name" }
}
$libs = $resolved
Write-Host "kit paths OK (SDK $sdkVer, UMDF $umdf); $($libs.Count) libs" -ForegroundColor Green

$incArgs = $inc | ForEach-Object { "/I$_" }

$cldefs = @(
    '/D_UNICODE','/DUNICODE',
    '/D_WIN32_WINNT=0x0A00',
    '/DUMDF_USING_NTSTATUS',
    '/DUMDF_VERSION_MAJOR=2',
    "/DUMDF_VERSION_MINOR=$($umdf.Split('.')[1])",
    '/DWIN32_NO_STATUS',
    '/DNTDDI_VERSION=0x0A000010'
)

$src = Join-Path $root 'hydrakbd-umdf\HydraKbdUmdf.cpp'
$obj = Join-Path $outdir 'HydraKbdUmdf.obj'
$dll = Join-Path $outdir 'HydraKbdUmdf.dll'

Write-Host "compiling HydraKbdUmdf.cpp ..." -ForegroundColor Cyan
$compile = @('/nologo','/c','/EHsc','/std:c++17','/W3','/MT') + $cldefs + $incArgs + @($src,"/Fo:$obj")
$out = & cl.exe @compile 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "COMPILE FAILED:" -ForegroundColor Red
    $out | Where-Object { $_ -match ': (error|fatal error) ' } | Select-Object -First 20 |
           ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
    Write-Error "compile stage failed"
}
Write-Host "  compiled OK" -ForegroundColor Green

Write-Host "linking HydraKbdUmdf.dll ..." -ForegroundColor Cyan
$res = Join-Path $outdir 'HydraKbdUmdf.res'
& rc.exe /nologo /fo $res (Join-Path $root 'hydrakbd-umdf\HydraKbdUmdf.rc')
$link = @('/nologo','/DLL',"/OUT:$dll",$obj,$res) + $libs +
        @('/NODEFAULTLIB:kernel32.lib','/SUBSYSTEM:WINDOWS,10.0')
$out = & link.exe @link 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "LINK FAILED:" -ForegroundColor Red
    $out | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
    Write-Error "link stage failed"
}
Write-Host "  linked OK" -ForegroundColor Green

Copy-Item (Join-Path $root 'hydrakbd-umdf\HydraKbdUmdf.inf') $outdir -Force

# UmdfLibraryVersion substitution in the COPY, never the source.
$infOut = Join-Path $outdir 'HydraKbdUmdf.inf'
$infTxt = [System.IO.File]::ReadAllText($infOut)
if ($infTxt -match [regex]::Escape('$UMDFVERSION$')) {
    $infTxt = $infTxt -replace [regex]::Escape('$UMDFVERSION$'), "$umdf.0"
    [System.IO.File]::WriteAllText($infOut, $infTxt, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "  UmdfLibraryVersion -> $umdf.0" -ForegroundColor Green
} else {
    Write-Warning "no UMDFVERSION token in $infOut"
}

$check = Select-String -Path $infOut -Pattern '^\s*UmdfLibraryVersion\s*=' |
         Select-Object -First 1 -ExpandProperty Line
if (-not $check)       { Write-Error "no UmdfLibraryVersion line in $infOut" }
if ($check -match '\$'){ Write-Error "UmdfLibraryVersion still holds a literal token: $check" }
Write-Host "  $($check.Trim())" -ForegroundColor DarkGray

Write-Host ""
Write-Host "Built: $dll" -ForegroundColor Green
Get-Item $dll | ForEach-Object { "  {0:N0} bytes" -f $_.Length }
Write-Host "Next: stampinf + inf2cat + .\sign-driver.ps1 -DriverDir .\dist\hydrakbd-umdf -CatalogName HydraKbdUmdf.cat" -ForegroundColor Cyan
