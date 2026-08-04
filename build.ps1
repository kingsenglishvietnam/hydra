# build.ps1 -- build the SDK-buildable Hydra components into .\dist
#
# Run from an "x64 Native Tools Command Prompt for VS 2022" (so cl.exe is on
# PATH), then:  powershell -ExecutionPolicy Bypass -File .\build.ps1
#
# Or from a bare PowerShell prompt -- this script self-arms the VS x64 dev shell
# if cl.exe isn't already visible.
#
# Builds: seat_router, seatB_agent, clip_console, respawn (pure Win32 C),
#         hydractl, hydrad, mirror (C++ / Win32 + D3D + SwDevice/WTS).
# Does NOT build iddseat.dll -- the IddCx display driver needs the WDK and a
# test-signed catalog. See BUILD.md, then drop iddseat.dll into .\dist.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'

# --- make sure cl.exe is available; if not, enter the VS x64 dev shell --------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Write-Error "cl.exe not found and vswhere.exe is missing. Open an 'x64 Native Tools Command Prompt for VS 2022' and run this from there."
    }

    # -products * matters: vswhere hides Build Tools installs by default, and
    # returns nothing (not an error) when no instance matches -- which then makes
    # Join-Path explode on a null path. Ask for an instance that actually has the
    # x64 C++ toolset; fall back to any instance; then give up loudly.
    $vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    $vs = & $vswhere -latest -products * -requires $vcTools -property installationPath 2>$null |
          Select-Object -First 1
    if (-not $vs) {
        $vs = & $vswhere -latest -products * -property installationPath 2>$null |
              Select-Object -First 1
    }
    if (-not $vs) {
        Write-Error @"
vswhere found no Visual Studio instance with the C++ toolset.
Install 'Desktop development with C++' (VS 2022 or Build Tools 2022),
or open an 'x64 Native Tools Command Prompt for VS 2022' and re-run there.
"@
    }

    $devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $devShell)) {
        Write-Error "found VS at '$vs' but no DevShell module at '$devShell'. Open an 'x64 Native Tools Command Prompt for VS 2022' and run this from there."
    }

    Write-Host "cl.exe not on PATH; entering VS x64 dev shell ($vs)..." -ForegroundColor Yellow
    Import-Module $devShell
    # -SkipAutomaticLocation keeps our cwd; -arch=x64 is what makes cl target 64-bit.
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
                     -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "still no cl.exe -- is the 'Desktop development with C++' workload installed?"
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null
Write-Host "Output: $dist" -ForegroundColor Cyan

$failed = @()
$built  = @()

# Invoke cl and ACTUALLY CHECK whether it worked. cl is a native exe: it signals
# failure with a nonzero exit code, which neither $ErrorActionPreference nor a
# pipe to Out-Null will surface. Check $LASTEXITCODE, or build nothing quietly.
function Invoke-Cl {
    param([string]$Label, [string[]]$Arguments, [string]$Expect)

    Write-Host ("cl  {0}" -f $Label)
    $out = & cl.exe @Arguments 2>&1
    $code = $LASTEXITCODE

    if ($code -ne 0 -or -not (Test-Path $Expect)) {
        $script:failed += $Label
        Write-Host "    FAILED (exit $code)" -ForegroundColor Red
        # Show only the diagnostic lines; cl is chatty about filenames.
        $out | Where-Object { $_ -match ': (error|fatal error) ' } |
               Select-Object -First 12 |
               ForEach-Object { Write-Host "      $_" -ForegroundColor DarkRed }
        return $false
    }
    $script:built += (Split-Path $Expect -Leaf)
    return $true
}

# --- pure Win32 C helpers (the verified v3 input stack) ---------------------
# Narrow main(); console subsystem; static CRT so dist\ is copy-deployable.
$cHelpers = @(
    @{ src = 'input\seat_router.c';  out = 'seat_router.exe';  libs = @('ws2_32.lib','user32.lib','winmm.lib') },
    @{ src = 'input\seatB_agent.c';  out = 'seatB_agent.exe';  libs = @('ws2_32.lib','user32.lib','winmm.lib','advapi32.lib') },
    @{ src = 'input\clip_console.c'; out = 'clip_console.exe'; libs = @('user32.lib') },
    @{ src = 'input\respawn.c';      out = 'respawn.exe';      libs = @() },
    @{ src = 'audio\audiotest.c';     out = 'audiotest.exe';     libs = @('ole32.lib','mmdevapi.lib','wtsapi32.lib') },
    @{ src = 'audio\session_route.c'; out = 'session_route.exe'; libs = @('ole32.lib','mmdevapi.lib','wtsapi32.lib') },
    @{ src = 'audio\mutetest.c';     out = 'mutetest.exe';     libs = @('ole32.lib','mmdevapi.lib') },
    @{ src = 'audio\route_endpoint.c'; out = 'route_endpoint.exe'; libs = @('ole32.lib','mmdevapi.lib') },
    @{ src = 'audio\audio_keepalive.c'; out = 'audio_keepalive.exe'; libs = @('ole32.lib') },
    @{ src = 'audio\audio_bridge.c';    out = 'audio_bridge.exe';    libs = @('ole32.lib') }
)

# seat_router.c needs the Interception SDK. Warn clearly instead of dying in cl.
$hasInterception = $false
foreach ($p in ($env:INCLUDE -split ';')) {
    if ($p -and (Test-Path (Join-Path $p 'interception.h'))) { $hasInterception = $true; break }
}
if (-not $hasInterception -and (Test-Path (Join-Path $root 'input\interception.h'))) {
    $hasInterception = $true
}

foreach ($h in $cHelpers) {
    if ($h.out -eq 'seat_router.exe' -and -not $hasInterception) {
        Write-Host "cl  $($h.src) -> SKIPPED" -ForegroundColor Yellow
        Write-Host "      interception.h not found on INCLUDE or in input\." -ForegroundColor Yellow
        Write-Host "      Get the Interception SDK, put interception.h + interception.lib" -ForegroundColor Yellow
        Write-Host "      in input\, then re-run. (Everything else builds without it.)" -ForegroundColor Yellow
        $failed += "$($h.src) (no Interception SDK)"
        continue
    }
    $args = @('/nologo','/O2','/W3','/MT',
              "/I$root\input",
              (Join-Path $root $h.src),
              "/Fo:$dist\", "/Fe:$(Join-Path $dist $h.out)",
              '/link', "/LIBPATH:$root\input") + $h.libs
    if ($h.out -eq 'seat_router.exe') { $args += 'interception.lib' }
    Invoke-Cl -Label $h.src -Arguments $args -Expect (Join-Path $dist $h.out) | Out-Null
}

# --- C++ pieces: control CLI, daemon, presenter -----------------------------
# wmain() entry points; /EHsc for C++ exceptions; static CRT.
$cppTargets = @(
    @{ src = 'hydractl\hydractl.cpp'; out = 'hydractl.exe'; libs = @() },
    @{ src = 'hydrad\hydrad.cpp';     out = 'hydrad.exe';
       libs = @('onecore.lib','wtsapi32.lib','userenv.lib','advapi32.lib','shell32.lib','ole32.lib','uuid.lib') },
    @{ src = 'mirror\mirror.cpp';     out = 'mirror.exe';
       libs = @('ws2_32.lib','d3d11.lib','dxgi.lib','user32.lib') },
    @{ src = 'capture\session_capture.cpp'; out = 'session_capture.exe';
       libs = @('d3d11.lib','dxgi.lib','user32.lib') }
)

foreach ($t in $cppTargets) {
    $args = @('/nologo','/O2','/W3','/EHsc','/MT','/std:c++17','/I',$root,
              (Join-Path $root $t.src),
              "/Fo:$dist\", "/Fe:$(Join-Path $dist $t.out)",
              '/link') + $t.libs
    Invoke-Cl -Label $t.src -Arguments $args -Expect (Join-Path $dist $t.out) | Out-Null
}

# --- stage config + inf so dist\ is runnable ---------------------------------
Copy-Item (Join-Path $root 'seats.toml')          $dist -Force

# Deploy interception.dll beside the exes so seat_router/seatB_agent load at run
# time (the .lib is only the link stub; the DLL is the actual code).
$idll = 'C:\Programs\Interception\library\x64\interception.dll'
if (Test-Path $idll) { Copy-Item $idll $dist -Force }

Copy-Item (Join-Path $root 'iddseat\iddseat.inf') $dist -Force
Get-ChildItem $dist -Filter *.obj -File -ErrorAction SilentlyContinue | Remove-Item -Force

# --- report -----------------------------------------------------------------
Write-Host ""
if ($built.Count) {
    Write-Host "Built ($($built.Count)):" -ForegroundColor Green
    Get-ChildItem $dist -Filter *.exe | ForEach-Object { "  {0,-18} {1,8:N0} bytes" -f $_.Name, $_.Length }
}
if ($failed.Count) {
    Write-Host ""
    Write-Host "FAILED ($($failed.Count)):" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Build incomplete." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Still needed: iddseat.dll (WDK build, test-signed). See BUILD.md." -ForegroundColor Yellow
