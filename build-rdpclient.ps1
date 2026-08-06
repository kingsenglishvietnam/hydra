# build-rdpclient.ps1 -- build Hydra's own headless RDP client.
#
# MILESTONE 1: connects and counts frames. Publishes nothing yet. The point is to
# prove the build chain, which is the actual risk in this project -- the code is
# straightforward, but libfreerdp on Windows means pinning a version and tracking
# an API that moves between minor releases.
#
# WHY MINGW AND NOT MSVC
#   libfreerdp is already installed on this machine via MSYS2, built with MinGW.
#   Linking MSVC objects against MinGW libraries is a fight with no purpose, so
#   this one target uses MSYS2's gcc while everything else in Hydra stays on
#   cl.exe. That is a deliberate split, not an oversight.
#
# PREREQUISITE -- the development headers, not just the runtime:
#   In an MSYS2 MINGW64 shell:
#     pacman -S mingw-w64-x86_64-freerdp mingw-w64-x86_64-pkg-config
#
# USAGE:
#   .\build-rdpclient.ps1

param(
    [string]$Msys = 'C:\msys64',
    [string]$Out  = "$PSScriptRoot\dist"
)

$ErrorActionPreference = 'Stop'

# gcc needs its OWN runtime DLLs (libgmp, libisl, libmpc, libgcc...) which live
# beside it. Without mingw64\bin on PATH, gcc.exe fails to start at all -- and
# the error names missing libraries rather than a missing PATH entry, which is
# thoroughly misleading. PATH also resets with every new shell, so setting it by
# hand keeps being forgotten. Do it here.
$mingwBin = Join-Path $Msys 'mingw64\bin'
if ($env:PATH -notlike "*$mingwBin*") {
    $env:PATH = "$mingwBin;" + $env:PATH
    Write-Host "added $mingwBin to PATH for this build" -ForegroundColor DarkGray
}

$gcc = Join-Path $Msys 'mingw64\bin\gcc.exe'
$pkg = Join-Path $Msys 'mingw64\bin\pkg-config.exe'
if (-not (Test-Path $gcc)) { throw "no gcc at $gcc -- install MSYS2 and the mingw-w64-x86_64 toolchain" }

# Ask pkg-config what libfreerdp needs, rather than guessing flag names that
# change between releases.
$cflags = ''
$libs   = ''
if (Test-Path $pkg) {
    $env:PKG_CONFIG_PATH = Join-Path $Msys 'mingw64\lib\pkgconfig'
    foreach ($mod in @('freerdp3','freerdp-client3','winpr3',
                       'freerdp2','freerdp-client2','winpr2')) {
        $probe = & $pkg --exists $mod 2>$null; $ok = ($LASTEXITCODE -eq 0)
        if ($ok) {
            $cflags += ' ' + (& $pkg --cflags $mod)
            $libs   += ' ' + (& $pkg --libs   $mod)
        }
    }
}
if (-not $libs) {
    Write-Warning "pkg-config found nothing -- falling back to explicit flags."
    $cflags = "-I$Msys\mingw64\include\freerdp3 -I$Msys\mingw64\include\winpr3"
    $libs   = "-L$Msys\mingw64\lib -lfreerdp3 -lfreerdp-client3 -lwinpr3"
}

Write-Host "cflags:$cflags" -ForegroundColor DarkGray
Write-Host "libs:  $libs"   -ForegroundColor DarkGray

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$src = Join-Path $PSScriptRoot 'rdp\hydrardp.c'
$exe = Join-Path $Out 'hydrardp.exe'

# -D__STDC_NO_THREADS__ : WinPR's platform.h includes C11 <threads.h> when the
#   compiler advertises C11 support. MinGW-w64 advertises it but does not ship
#   the header, so the include fails before any of our code is even parsed.
#   Defining this tells WinPR to take its non-C11 path, which is what every other
#   MinGW build of FreeRDP does.
# -D_WIN32_WINNT=0x0A00 : match the rest of Hydra, so the same Win32 APIs are
#   visible here as everywhere else.
$defs = '-D__STDC_NO_THREADS__=1 -D_WIN32_WINNT=0x0A00'
# -lws2_32 for WSAStartup/getaddrinfo -- without Winsock initialised, FreeRDP
# cannot resolve even a literal IP and reports it as a DNS failure.
$cmd = "`"$gcc`" -O2 -Wall $defs $cflags `"$src`" -o `"$exe`" $libs -lws2_32"
Write-Host "gcc  rdp\hydrardp.c" -ForegroundColor Cyan
cmd /c $cmd
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Build failed. On a first link against libfreerdp this is normal --" -ForegroundColor Yellow
    Write-Host "usually a header path or a renamed symbol. Paste the errors." -ForegroundColor Yellow
    exit 1
}

Write-Host "Built: $exe" -ForegroundColor Green
Write-Host ""
Write-Host "Test (nothing is published yet -- it only counts frames):" -ForegroundColor Cyan
Write-Host "  .\dist\hydrardp.exe B teacher"
Write-Host ""
Write-Host "Expect:  connected; GDI ready" -ForegroundColor DarkGray
Write-Host "         desktop is 1920x1080, 32 bpp" -ForegroundColor DarkGray
Write-Host "         seat B: N paints so far" -ForegroundColor DarkGray
Write-Host ""
Write-Host "It needs MSYS2's DLLs on PATH:" -ForegroundColor Yellow
Write-Host "  `$env:PATH = '$Msys\mingw64\bin;' + `$env:PATH"
