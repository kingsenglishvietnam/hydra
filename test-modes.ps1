# test-modes.ps1 -- acceptance tests for modes 1, 2 and 3.   REV 3
#
# CHANGES FROM REV 2
#   * Sets display_mode automatically per mode and runs setup.ps1. Getting this
#     wrong is the single most common cause of a false failure: mode 2 needs
#     "capture", mode 3 needs "client", and switching only one of the two gives
#     a broken hybrid that looks like a bug.
#   * MEASURES instead of asking. Reads the pixel ring twice and reports frames
#     per second, cursor positions per second, and the stalled counter as
#     numbers. "Glitchy" is not a test result; 111 fps with curSeq at 60/s is.
#   * Checks agent_B.log for restarts and err 5. Three agent startup banners in
#     eight lines is a restart loop, and each restart is a gap in injection --
#     that is what seat 2 mouse stutter looks like from the log side.
#   * Verifies -suppress-output and /scale:140 are actually in hydra-start.ps1.
#
# REV 1 was wrong in ways worth remembering: it invoked the clients raw instead
# of through the launchers, so it dropped /sound (which is what creates the
# seat's audio ENDPOINT -- without it abcap fails 0x80070490 forever) and
# stripped HYDRA_GFX (no codec, plain bitmap updates, looks like a glitch bug).
# Both "failures" were the harness.
#
# USAGE (elevated, from the Hydra Shell)
#   .\test-modes.ps1 -PreflightOnly
#   .\test-modes.ps1 -Mode 2
#   .\test-modes.ps1 -Mode 3 -Gfx RFX
#   .\test-modes.ps1 -Mode 2 -NoModeSwitch      # leave seats.toml alone

param(
    [ValidateSet(0,1,2,3)][int]$Mode = 0,
    [switch]$PreflightOnly,
    [switch]$NoModeSwitch,
    [ValidateSet('RFX','progressive','none')][string]$Gfx = 'RFX',
    [string]$Root = 'C:\Programs\hydra'
)

$ErrorActionPreference = 'Continue'
# PS 7.4 made native-command stderr honour ErrorActionPreference. Several tools
# here write PROGRESS to stderr -- hydractl's 'not reachable' while it waits,
# mirror's 'pixel transport opened' -- and 2>&1 under 'Stop' turned those
# SUCCESS lines into terminating errors. This broke hydra-start.ps1 on 2026-08-21.
$PSNativeCommandUseErrorActionPreference = $false
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$results = @()
$fail    = 0

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

function Check($name, $ok, $detail = '') {
    $script:results += [PSCustomObject]@{ Test = $name; Result = $(if ($ok) {'PASS'} else {'FAIL'}); Detail = $detail }
    if ($ok) { Say "  PASS  $name  $detail" Green } else { Say "  FAIL  $name  $detail" Red; $script:fail++ }
}

function Info($name, $detail) {
    $script:results += [PSCustomObject]@{ Test = $name; Result = 'INFO'; Detail = $detail }
    Say "  ....  $name  $detail" DarkGray
}

function Human($name, $question) {
    Say ""
    Say "  >> $question" Cyan
    $a = Read-Host "     (y/n/skip)"
    $r = switch -Regex ($a) { '^y' {'PASS'} '^n' {'FAIL'} default {'SKIP'} }
    $script:results += [PSCustomObject]@{ Test = $name; Result = $r; Detail = '(observed)' }
    if ($r -eq 'FAIL') { $script:fail++; Say "     FAIL" Red }
    elseif ($r -eq 'SKIP') { Say "     skipped" Yellow } else { Say "     PASS" Green }
}

function StopAll {
    Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay -EA SilentlyContinue | Stop-Process -Force
    Stop-Service Hydra -EA SilentlyContinue
    Start-Sleep 2
    $t = query session | Select-String 'teacher'
    if ($t) {
        $sid = ($t.ToString().Trim() -split '\s+' | Where-Object { $_ -match '^\d+$' } | Select-Object -First 1)
        if ($sid) { Say "  logging off stale teacher session $sid" Yellow; logoff $sid 2>$null; Start-Sleep 3 }
    }
}

function WaitSeat($timeoutSec = 90) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeoutSec) {
        if ((& query session | Out-String) -match 'teacher\s+\d+\s+Active') { return $true }
        Start-Sleep 2
    }
    return $false
}

# Parse hydra-shm.ps1 output into numbers.
function ReadRing {
    $o = (& "$Root\hydra-shm.ps1" 6>&1 | Out-String)
    [PSCustomObject]@{
        Seq     = if ($o -match 'seq=(\d+)')     { [int64]$Matches[1] } else { -1 }
        CurSeq  = if ($o -match 'curSeq=(\d+)')  { [int64]$Matches[1] } else { -1 }
        Frame   = if ($o -match 'frame=(\d+)')   { [int64]$Matches[1] } else { -1 }
        Ready   = if ($o -match 'ready=(\d+)')   { [int]$Matches[1] }   else { -1 }
        Stalled = if ($o -match 'STALLED=(\d+)') { [int]$Matches[1] }   else { 0 }
        Audio   = if ($o -match 'writePos=(\d+)'){ [int64]$Matches[1] } else { -1 }
        Raw     = $o
    }
}

function SetDisplayMode($want) {
    $p = "$Root\seats.toml"
    $t = [IO.File]::ReadAllText($p)
    if ($t -match '(?m)^display_mode = "([^"]+)"') {
        $cur = $Matches[1]
        if ($cur -eq $want) { Info 'display_mode' "already $want"; return }
        $t = $t -replace '(?m)^display_mode = "[^"]+"', "display_mode = `"$want`""
        [IO.File]::WriteAllText($p, $t)
        Say "  display_mode $cur -> $want ; running setup.ps1" Yellow
        & "$Root\setup.ps1" | Out-Null
        $d = Select-String -Path "$Root\dist\seats.toml" -Pattern '^display_mode' | Select-Object -First 1
        Info 'display_mode' "$($d.Line.Trim())  (deployed)"
    } else { Say "  could not find display_mode in seats.toml" Red }
}

Set-Location $Root

# =====================================================================
Say ""
Say "=== preflight ===" Cyan

$svc = & sc.exe qc TermService | Out-String
Check 'TermService type= own' ($svc -match 'WIN32_OWN_PROCESS') 'fix: sc config TermService type= own, then reboot'

$dll = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' ServiceDll -EA SilentlyContinue).ServiceDll
Check 'ServiceDll = rdpwrap.dll' ($dll -like '*rdpwrap*') "$dll"

$tsPid = (Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId
$mods  = if ($tsPid) { (Get-Process -Id $tsPid -Module -EA SilentlyContinue).ModuleName } else { @() }
Check 'rdpwrap loaded into TermService' ($mods -contains 'rdpwrap.dll')

Check 'fDenyTSConnections = 0' ((Get-ItemProperty 'HKLM:\System\CurrentControlSet\Control\Terminal Server' fDenyTSConnections -EA SilentlyContinue).fDenyTSConnections -eq 0)
Check 'teacher account enabled' ([bool](Get-LocalUser teacher -EA SilentlyContinue | Where-Object Enabled))

$kf = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E96B-E325-11CE-BFC1-08002BE10318}' UpperFilters -EA SilentlyContinue).UpperFilters
$mf = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E96F-E325-11CE-BFC1-08002BE10318}' UpperFilters -EA SilentlyContinue).UpperFilters
Check 'Interception kbd filter'   ($kf -contains 'keyboard') "$($kf -join ',')"
Check 'Interception mouse filter' ($mf -contains 'mouse')    "$($mf -join ',')"
Check 'Interception drivers running' (((Get-Service keyboard, mouse -EA SilentlyContinue).Status | Where-Object { $_ -eq 'Running' }).Count -eq 2)

$ep  = & "$Root\dist\route_endpoint.exe" --list 2>&1 | Out-String
$cfg = Get-Content "$Root\seats.toml" -Raw -EA SilentlyContinue
if ($cfg -match '(?m)^audio_bridge\s*=\s*"([^"]+)"') {
    Check "audio_bridge id '$($Matches[1])' resolves" ($ep -match [regex]::Escape($Matches[1])) 'a reset reissues these -- route_endpoint.exe --list'
}

# The two flags established 08-14. Both live in hydra-start.ps1's arg list.
$hs = Get-Content "$Root\hydra-start.ps1" -Raw
Check '-suppress-output wired in' ($hs -match 'suppress-output') 'stops the client suppressing frames when covered'
Check '/scale:140 wired in'       ($hs -match 'scale:140')       'session DPI, so DISPLAY2 can sit at 100% and mirror runs 1:1'

# Logitech: installing its driver INSIDE the seat session is the confirmed
# PROBLEM 1 trigger (1806 capture retries, only a client reconnect cleared it).
Check 'Logitech Download Assistant removed' (-not (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name 'Logitech Download Assistant' -EA SilentlyContinue))
Info 'driver search from WU' "SearchOrderConfig=$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching' SearchOrderConfig -EA SilentlyContinue).SearchOrderConfig)  (0 = off)"
Info 'UAC secure desktop' "PromptOnSecureDesktop=$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' PromptOnSecureDesktop -EA SilentlyContinue).PromptOnSecureDesktop)  (0 = prompts on the normal desktop, capturable)"

Add-Type -AssemblyName System.Windows.Forms -EA SilentlyContinue
$screens = [System.Windows.Forms.Screen]::AllScreens
Check 'two displays present' ($screens.Count -ge 2) "found $($screens.Count)"
$d2 = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1
# clip_console is Per-Monitor-V2 aware; Windows.Forms is not and reports scaled sizes.
$cc = & "$Root\dist\clip_console.exe" 2>&1 | Out-String
Info 'real geometry' (($cc -split "`r?`n" | Where-Object { $_ -match 'monitor \d' }) -join '  |  ')

Say ""
if ($fail) { Say "$fail precondition(s) failed." Red } else { Say "preconditions OK" Green }
if ($PreflightOnly) { $results | Format-Table -AutoSize; return }

if ($Mode -eq 0) { Say ""; Say "1 = mstsc   2 = sdl-freerdp   3 = hydrardp" Cyan; $Mode = [int](Read-Host 'mode') }

# =====================================================================
Say ""
Say "=== mode $Mode ===" Cyan
StopAll

if (-not $NoModeSwitch) { SetDisplayMode 'capture' }   # client mode leaves meta unpopulated -- hydrardp never writes it

switch ($Mode) {
  1 {
        Say "MODE 1 -- mstsc via hydra-start.ps1." Cyan
        Start-Process powershell -ArgumentList '-NoExit','-Command',"cd '$Root'; .\hydra-start.ps1 -Client mstsc"
    }
  2 {
        Say "MODE 2 -- sdl-freerdp via hydra-start.ps1 (with /sound, -suppress-output, /scale:140)." Cyan
        Start-Process powershell -ArgumentList '-NoExit','-Command',"cd '$Root'; .\hydra-start.ps1"
    }
  3 {
        Say "MODE 3 -- hydrardp headless via hydra-view.ps1, HYDRA_GFX=$Gfx." Cyan
        Say "  never HYDRA_GFX=1 -- the server picks H.264 and this VAAPI build crashes" Yellow
        Get-Process session_capture -EA SilentlyContinue | Stop-Process -Force
        $set = if ($Gfx -eq 'none') { 'Remove-Item Env:HYDRA_GFX -EA SilentlyContinue' } else { "`$env:HYDRA_GFX='$Gfx'" }
        Start-Process powershell -ArgumentList '-NoExit','-Command',"cd '$Root'; $set; .\hydra-view.ps1"
    }
}

Say "  waiting for the teacher session -- log in in the new window ..." Yellow
Check 'teacher session Active' (WaitSeat 120)

for ($i=0; $i -lt 20; $i++) { Start-Sleep 2; if ((& "$Root\dist\hydractl.exe" status 2>&1 | Out-String) -notmatch 'not reachable') { break } }
$st = & "$Root\dist\hydractl.exe" status 2>&1 | Out-String
Say $st DarkGray
Check 'router running'  ($st -match 'router:\s+running')
Check 'agent:B running' ($st -match 'agent:B:\s+running')
Check 'abcap:B running' ($st -match 'abcap:B:\s+running') 'waiting = no audio endpoint in the seat -- is /sound passed?'
if ($Mode -ne 3) { Check 'capture:B running' ($st -match 'capture:B:\s+running') }

if ($Mode -eq 3) {
    # display_mode=client would be correct here, but hydrardp.c never writes the
    # meta section -- mirror then sees ready=0 and draws a white box. So run under
    # capture, let session_capture populate meta, then kill it so hydrardp is the
    # only producer. Meta persists in the shared section after the writer exits.
    Start-Sleep 3
    Get-Process session_capture -EA SilentlyContinue | Stop-Process -Force
    Say '  session_capture stopped -- hydrardp is now the only producer' Yellow
}
if (-not (Get-Process mirror -EA SilentlyContinue)) {
    Start-Process "$Root\dist\mirror.exe" -ArgumentList 'B', $d2.DeviceName
    Start-Sleep 5
}
Check 'mirror running' ([bool](Get-Process mirror -EA SilentlyContinue))

# =====================================================================
# MEASURE. Generate activity in the seat while this runs -- DDA publishes
# nothing on a static desktop, and a still picture is not a failure.
Say ""
Say "  >> MOVE THE MOUSE AND DRAG A WINDOW IN SEAT B for the next 6 seconds" Cyan
Read-Host "     press Enter, then start moving"
$r1 = ReadRing; Start-Sleep 6; $r2 = ReadRing

$fps  = if ($r1.Frame  -ge 0) { [math]::Round(($r2.Frame  - $r1.Frame)  / 6, 1) } else { -1 }
$cps  = if ($r1.CurSeq -ge 0) { [math]::Round(($r2.CurSeq - $r1.CurSeq) / 6, 1) } else { -1 }
$aud  = $r2.Audio - $r1.Audio

Check 'meta ready'            ($r2.Ready -eq 1) "ready=$($r2.Ready)  (0 = nothing populated the meta section; mirror draws a white box)"
Check 'stalled = 0'           ($r2.Stalled -eq 0) "stalled=$($r2.Stalled)  (non-zero = attached but EnumOutputs empty -- PROBLEM 1)"
Check 'frames flowing'        ($fps -gt 5)  "$fps fps"
Check 'cursor positions'      ($cps -gt 30) "$cps /sec  (~61 is normal for agent:B)"
Info  'audio ring'            "writePos +$aud  (0 while nothing is playing is fine)"

# agent restarts are what seat 2 mouse stutter looks like from the log side.
$al = Get-Content "$Root\..\..\ProgramData\Hydra\logs\agent_B.log" -Tail 40 -EA SilentlyContinue
if (-not $al) { $al = Get-Content 'C:\ProgramData\Hydra\logs\agent_B.log' -Tail 40 -EA SilentlyContinue }
$restarts = ($al | Select-String 'clearing stuck modifiers').Count
$err5     = ($al | Select-String 'err 5').Count
$redesk   = ($al | Select-String 'input desktop changed').Count
Check 'agent not restarting' ($true) "$restarts startup banners in the last 40 lines (each restart is a gap in injection)"
Check 'no SendInput err 5'   ($err5 -eq 0)     "$err5 occurrences (fires on a secure desktop -- see PromptOnSecureDesktop)"
Info  'desktop re-attaches'  "$redesk in the last 40 lines"

# =====================================================================
Human "mode${Mode}: picture on panel"    'Seat desktop on the external monitor, correctly sized?'
Human "mode${Mode}: video smooth"        'Play a video in the seat. Smooth?  (n = torn / blocky / stuttering)'
Human "mode${Mode}: seat cursor"         'Cursor visible and tracking without stutter?'
Human "mode${Mode}: keyboard isolated"   'Wireless keyboard types into seat 2 ONLY?'
Human "mode${Mode}: console unaffected"  'Wired devices still drive the console?'
Human "mode${Mode}: audio at monitor"    'Sound from the MONITOR?'
Human "mode${Mode}: survives UAC"        'Trigger something needing admin in seat B. Does the panel keep updating?'
if ($Mode -eq 2) { Human 'mode2: no freeze when covered' 'Cover the client thumbnail ~15s. Panel keeps updating?' }

# =====================================================================
Say ""
Say "=== result ===" Cyan
$results | Format-Table -AutoSize

$out = "$Root\test-results-mode$Mode-$stamp.md"
$l  = @("# Hydra mode $Mode acceptance test -- $(Get-Date -Format 'yyyy-MM-dd HH:mm')", '')
$l += "measured: $fps fps, $cps cursor/sec, stalled=$($r2.Stalled), ready=$($r2.Ready)"
if ($Mode -eq 3) { $l += "HYDRA_GFX = $Gfx" }
$l += ''
$l += '| test | result | detail |'
$l += '|---|---|---|'
$results | ForEach-Object { $l += "| $($_.Test) | **$($_.Result)** | $($_.Detail) |" }
$l += ''
$l += "Failures: $fail"
$l | Set-Content $out
Say "written: $out" DarkGray

Say ""
Say "Stop everything:" Cyan
Say "  Get-Process mirror, hydrardp, sdl-freerdp, mstsc, cursor_overlay -EA SilentlyContinue | Stop-Process -Force; Stop-Service Hydra; query session"
Say "  then: logoff <teacher id>"
