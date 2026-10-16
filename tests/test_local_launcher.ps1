$ErrorActionPreference = 'Stop'
Import-Module (Join-Path (Split-Path $PSScriptRoot -Parent) 'local\HydraLocal.psm1') -Force
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    "PASS: $Message"
}
function Reject([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Check $failed $Message
}
$windows = @'
  monitor 0: (0,0)-(1920,1200)  \\.\DISPLAY1
  monitor 1: (-1920,0)-(0,1080)  \\.\DISPLAY2
  monitor 2: (1920,0)-(3840,1200)  \\.\DISPLAY3
  monitor 3: (3840,-100)-(5760,980)  \\.\DISPLAY4
'@
$clients = @'
listing 4 monitors:
     * [7] [CG2420] 1920x1200 +0+0
       [3] [T24v-10] 1920x1080 +-1920+0
       [9] [CG2420] 1920x1200 +1920+0
       [1] [T24v-10] 1920x1080 +3840+-100
'@
$cfg = [pscustomobject]@{
    console_monitor = '\\.\DISPLAY1'
    seats = @(
        [pscustomobject]@{name='B';session='user:seat-b';monitor='\\.\DISPLAY2';display_mode='off'},
        [pscustomobject]@{name='C';session='user:seat-c';monitor='\\.\DISPLAY3';display_mode='off'},
        [pscustomobject]@{name='D';session='user:seat-d';monitor='\\.\DISPLAY4';display_mode='off'}
    )
}
$b = New-HydraLocalPlan $cfg B $windows $clients
$c = New-HydraLocalPlan $cfg C $windows $clients
$d = New-HydraLocalPlan $cfg D $windows $clients
Check ($b.ClientMonitorId -eq 3 -and $c.ClientMonitorId -eq 9 -and $d.ClientMonitorId -eq 1) 'three seats map by geometry, not duplicate monitor names or DISPLAY numbers'
Check ($b.Arguments -contains '/v:127.0.0.2') 'client stays on this PC'
Check ($b.Arguments -contains '/u:seat-b' -and $b.Arguments -contains '/monitors:3') 'user and physical display remain seat-specific'
Check (-not ($b.Arguments | Where-Object { $_ -like '/p:*' })) 'no password appears in command line'
Reject { New-HydraLocalPlan $cfg C $windows ($clients + "`n       [10] [CG2420] 1920x1200 +1920+0") } 'ambiguous display geometry rejected'
Reject { New-HydraLocalPlan $cfg C $windows ($clients -replace '\+1920\+0', '+9999+0') } 'missing matching display rejected'
Reject { New-HydraLocalPlan $cfg A $windows $clients } 'unknown seat rejected'
$cfg.seats[0].session = 'auto'
Reject { New-HydraLocalPlan $cfg B $windows $clients } 'automatic session cannot choose a client user'
$cfg.seats[0].session = 'user:seat-b'
$cfg.seats[0].monitor = '\\.\DISPLAY1'
Reject { New-HydraLocalPlan $cfg B $windows $clients } 'console monitor cannot be taken by another seat'

$path = 'C:\Hydra\dist\freerdp\sdl-freerdp.exe'
$saved = [pscustomobject]@{Id=123;StartUtcTicks='639000000000000000';Path=$path;ConsoleSession=1}
$current = [pscustomobject]@{Id=123;StartUtcTicks='639000000000000000';Path=$path;ConsoleSession=1}
Check (Test-HydraClientIdentity $saved $current $path 1) 'owned process identity accepted'
$current.StartUtcTicks = '639000000000000001'
Check (-not (Test-HydraClientIdentity $saved $current $path 1)) 'reused PID cannot stop another process'
$current.StartUtcTicks = $saved.StartUtcTicks
$current.Path = 'C:\unrelated.exe'
Check (-not (Test-HydraClientIdentity $saved $current $path 1)) 'unrelated executable cannot be stopped'
$current.Path = $path; $current.ConsoleSession = 2
Check (-not (Test-HydraClientIdentity $saved $current $path 1)) 'another Windows session cannot be stopped'
Check ((ConvertTo-HydraArgument '') -ceq '""') 'empty argument quoted'
Check ((ConvertTo-HydraArgument 'C:\path with spaces\') -ceq '"C:\path with spaces\\"') 'terminal backslash survives Windows argument parsing'
Check ((ConvertTo-HydraArgument 'a"b') -ceq '"a\"b"') 'embedded quote cannot create a new argument'

# Parse the entry script too; do not execute its Start/Stop branches here.
$tokens = $null; $errors = $null
$script = Join-Path (Split-Path $PSScriptRoot -Parent) 'hydra-local.ps1'
$null = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
Check ($errors.Count -eq 0) 'local launcher parses under this PowerShell version'
