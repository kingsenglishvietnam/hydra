# build-mssample.ps1 -- compile Microsoft's IddCx sample with OUR build recipe.
#
# THE QUESTION THIS ANSWERS
#   0xD000000D (STATUS_INVALID_PARAMETER) at UMDF load level 0, before
#   DriverEntry, on both the console and remote builds. Eleven causes eliminated
#   as of 2026-08-16: remote adapter flag, hardware ID form,
#   UmdfHostProcessSharing, UmdfKernelModeClientPolicy, UMDF version (2.35/2.33),
#   IddCx version (1.11/1.10/1.2), UmdfExtensions absent, UmdfExtensions value,
#   IndirectKmd upper filter, ClassVer, ServiceBinary %13% vs %12%\UMDF. Plus
#   signing and the driverless path, ruled out 08-13/08-14.
#
#   So: is the fault in iddseat.cpp, or in this machine?
#
#   Microsoft's sample cannot be built by its own vcxproj here -- it has no WDK
#   include paths and dies on wudfwdm.h. But build-driver.ps1's paths are known
#   good; they compile our driver every time. Feeding the sample's Driver.cpp
#   through the SAME recipe holds the toolchain constant and changes only the
#   source.
#
#     Sample LOADS      -> the fault is in iddseat.cpp. A working reference to
#                          diff against, line by line.
#     Sample FAILS 2007 -> environmental. Nothing in our code will fix it, and
#                          five days of source-level work stops being the way in.
#
# USAGE (elevated, x64 Native Tools)
#   .\build-mssample.ps1
#   .\build-mssample.ps1 -Install     # also stamp, cat, sign and stage it

param(
    [string]$Sample  = 'C:\Programs\wdksample\video\IndirectDisplay\IddSampleDriver',
    [string]$OutDir  = 'C:\Programs\hydra\dist\mssample',
    [string]$KitRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$SdkVer  = '10.0.26100.0',
    [string]$IddCx   = '1.2',      # the only version registered on this box:
                                   # HKLM\...\Control\Wdf\UMDF\IddCx\Versions\1\2
                                   # has Service = IddCx0102
    [string]$Umdf    = '2.33',
    [switch]$Install
)

$ErrorActionPreference = 'Stop'

foreach ($p in @("$Sample\Driver.cpp", "$Sample\IddSampleDriver.inf")) {
    if (-not (Test-Path $p)) { throw "not found: $p" }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Same include set as build-driver.ps1. Note wudfwdm.h lives ONLY under
# Include\wdf\umdf\<ver>\ -- not under Include\<sdk>\km, whatever the comment in
# build-driver.ps1 says. That is why the sample's own vcxproj cannot find it.
$inc = @(
    "$KitRoot\Include\$SdkVer\um",
    "$KitRoot\Include\$SdkVer\shared",
    "$KitRoot\Include\$SdkVer\km",
    "$KitRoot\Include\$SdkVer\um\iddcx\$IddCx",
    "$KitRoot\Include\wdf\umdf\$Umdf"
)
foreach ($d in $inc) { if (-not (Test-Path $d)) { throw "missing include dir: $d" } }

$libs = @(
    "$KitRoot\Lib\$SdkVer\um\x64\iddcx\$IddCx\iddcxstub.lib",
    "$KitRoot\Lib\wdf\umdf\x64\$Umdf\WdfDriverStubUm.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\d3d11.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\dxgi.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\avrt.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\advapi32.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\kernel32.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\ole32.lib"
)
$ntdllUm = @(
    "$KitRoot\Lib\wdf\umdf\x64\$Umdf\ntdllUm.lib",
    "$KitRoot\Lib\$SdkVer\um\x64\ntdll.lib"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($ntdllUm) { $libs += $ntdllUm }

$missing = $libs | Where-Object { -not (Test-Path $_) }
if ($missing) { $missing | ForEach-Object { Write-Warning "lib not found: $_" } }
$libs = $libs | Where-Object { Test-Path $_ }

Write-Host "kit paths OK (SDK $SdkVer, IddCx $IddCx, UMDF $Umdf); $($libs.Count) libs" -ForegroundColor Green

# The sample is C++/WinRT-flavoured and uses WRL ComPtr; /std:c++17 and /EHsc
# match what our driver builds with.
$cldefs = @(
    '/D_UNICODE','/DUNICODE',
    '/D_WIN32_WINNT=0x0A00',
    '/DUMDF_USING_NTSTATUS',
        '/DNTDDI_VERSION=0x0A000010'
)

$obj = Join-Path $OutDir 'Driver.obj'
$dll = Join-Path $OutDir 'IddSampleDriver.dll'

Write-Host "compiling Microsoft's Driver.cpp ..." -ForegroundColor Cyan
$compile = @('/nologo','/c','/EHsc','/std:c++17','/W3','/MT') +
           $cldefs + ($inc | ForEach-Object { "/I$_" }) + @("$Sample\Driver.cpp","/Fo:$obj")
$out = & cl.exe @compile 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "COMPILE FAILED:" -ForegroundColor Red
    $out | Where-Object { $_ -match ': (error|fatal error) ' } | Select-Object -First 25 |
           ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
    Write-Host ""
    Write-Host "A compile failure here is ITSELF informative: the sample targets" -ForegroundColor Yellow
    Write-Host "IddCx 1.2 API, and if it will not build against this kit then the" -ForegroundColor Yellow
    Write-Host "kit or its headers are a suspect in their own right." -ForegroundColor Yellow
    throw "compile stage failed"
}
Write-Host "  compiled OK" -ForegroundColor Green

Write-Host "linking IddSampleDriver.dll ..." -ForegroundColor Cyan
$link = @('/nologo','/DLL',"/OUT:$dll",$obj) + $libs + @('/NODEFAULTLIB:kernel32.lib')
$out = & link.exe @link 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "LINK FAILED:" -ForegroundColor Red
    $out | Where-Object { $_ -match ': (error|fatal error) ' } | Select-Object -First 25 |
           ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
    throw "link stage failed"
}
Write-Host "  linked OK" -ForegroundColor Green

# The sample INF, with UmdfLibraryVersion substituted the way build-driver.ps1
# does for ours -- stampinf does NOT touch that directive (commit 1427b7a).
Copy-Item "$Sample\IddSampleDriver.inf" $OutDir -Force
$infOut = Join-Path $OutDir 'IddSampleDriver.inf'
$infTxt = [IO.File]::ReadAllText($infOut)
if ($infTxt -match [regex]::Escape('$UMDFVERSION$')) {
    $infTxt = $infTxt -replace [regex]::Escape('$UMDFVERSION$'), "$Umdf.0"
    [IO.File]::WriteAllText($infOut, $infTxt, (New-Object System.Text.UTF8Encoding $false))
}
Write-Host "  INF copied, UmdfLibraryVersion -> $Umdf.0" -ForegroundColor Green

Write-Host ""
Write-Host "Built: $dll" -ForegroundColor Green
Write-Host "  $((Get-Item $dll).Length) bytes  (ours is ~217,600)" -ForegroundColor DarkGray
Select-String -Path $infOut -Pattern 'UmdfExtensions|UmdfLibraryVersion|ServiceBinary|IndirectKmd' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor DarkGray }

if (-not $Install) {
    Write-Host ""
    Write-Host "Re-run with -Install to stamp, catalog, sign and stage it." -ForegroundColor Cyan
    return
}

Write-Host ""
Write-Host "=== staging ===" -ForegroundColor Cyan
& "$KitRoot\bin\$SdkVer\x64\stampinf.exe" -f $infOut -d * -a amd64 -v 1.0.0.1
& "$KitRoot\bin\$SdkVer\x86\Inf2Cat.exe" /driver:$OutDir /os:10_x64
& 'C:\Programs\hydra\sign-driver.ps1' -DriverDir $OutDir

Write-Host ""
Write-Host "Then, to actually create the device -- the sample ships its own app" -ForegroundColor Cyan
Write-Host "that calls SwDeviceCreate, so hydrad is not involved:" -ForegroundColor Cyan
Write-Host "  pnputil /add-driver $infOut /install"
Write-Host "  C:\Programs\wdksample\video\IndirectDisplay\x64\Release\IddSampleApp.exe"
Write-Host ""
Write-Host "Then read the verdict:" -ForegroundColor Cyan
Write-Host "  Get-WinEvent -LogName 'Microsoft-Windows-DriverFrameworks-UserMode/Operational' -MaxEvents 6 | Select-Object TimeCreated, Id, Message | Format-List"
Write-Host ""
Write-Host "  NO event 2007  -> the sample loads. The fault is in iddseat.cpp." -ForegroundColor Yellow
Write-Host "  event 2007     -> environmental. Our source is innocent." -ForegroundColor Yellow
Write-Host ""
Write-Host "CLEAN UP AFTERWARDS -- do not leave a stray IDD staged:" -ForegroundColor Cyan
Write-Host "  pnputil /enum-drivers | Out-String -Width 300 | Select-String 'IddSample'"
Write-Host "  pnputil /delete-driver oemNN.inf /uninstall"
