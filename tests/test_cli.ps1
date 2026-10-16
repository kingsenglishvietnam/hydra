# Integration checks of the compiled entry points. No live input capture.
param([string]$Dist = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
function Invoke-TestCommand([string]$Executable, [string[]]$Arguments, [int]$Expected) {
    $old = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue' # expected native stderr on rejection
        $text = (& $Executable @Arguments 2>&1 | Out-String)
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    if ($code -ne $Expected) { throw "Expected exit $Expected, got $code from $Executable`: $text" }
    return $text
}
$cfg = Invoke-TestCommand (Join-Path $Dist 'hydractl.exe') @('config', (Join-Path $root 'examples\four-local-seats.toml')) 0
$parsed = $cfg | ConvertFrom-Json
if ($parsed.seats.Count -ne 3 -or $parsed.seats[2].name -ne 'D') { throw 'Offline seat export failed.' }
'PASS: offline config export describes console + B/C/D without a service'

# Invalid configurations exit before the router opens its sockets or driver.
$router = Join-Path $Dist 'seat_router.exe'
Invoke-TestCommand $router @('--check', '--seat', '65535', '--kbd', '2', '--mouse', '12') 2 | Out-Null
Invoke-TestCommand $router @('--check', '--seat', '56789x', '--kbd', '2', '--mouse', '12') 2 | Out-Null
Invoke-TestCommand $router @('--check', '--seat', '56789', '--kbd', '2', '--mouse', '12',
                            '--seat', '57789', '--kbd', '3', '--mouse', '13') 2 | Out-Null
'PASS: production router rejects range, numeric-suffix and injector-port collisions'

# The agent rejects these before it attaches to a window station or emits input.
Invoke-TestCommand (Join-Path $Dist 'seatB_agent.exe') @('127.0.0.1', '56789', 'B\C') 2 | Out-Null
Invoke-TestCommand (Join-Path $Dist 'seatB_agent.exe') @('127.0.0.1', '56789junk', 'C') 2 | Out-Null
'PASS: production agent rejects malformed seat identity and port'
