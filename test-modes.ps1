# test-modes.ps1 -- acceptance tests for modes 1, 2 and 3.   REV 2
#
# WHAT WAS WRONG WITH REV 1
#   It invoked the clients raw instead of using the project's own launchers, and
#   every failure it "found" was its own doing:
#
#   * mode 2 "no audio" -- rev 1 ran sdl-freerdp without /sound.
#     hydra-start.ps1 line 160 says plainly: "/sound IS REQUIRED, even though
#     audio_bridge carries the audio... removing /sound removes the seat
#     session's audio ENDPOINT entirely". No endpoint means abcap fails
#     0x80070490 forever. The conclusion drawn from that -- that MSYS2's
#     FreeRDP has no rdpsnd backend and modes 2/3 structurally cannot have
#     audio -- was WRONG and got written into MODES.md and HANDOFF.md.
#     Delete it there.
#
#   * mode 3 "glitchy / nothing" -- rev 1 did Remove-Item Env:HYDRA_GFX, which
#     strips the codec and leaves plain bitmap updates. That line came from
#     STATE.md's manual sequence, which PREDATES commit 98ad249 (tag
#     gfx-working) where the /gfx crash was root-caused (update->DesktopResize
#     NULL) and the glitching fixed by publishing from the gfx EndFrame
#     callback instead of a blind 16ms timer. HYDRA_GFX=RFX is the working
#     setting. HYDRA_GFX=1 still crashes -- the server picks H.264 and this
#     libfreerdp is built WITH_VAAPI_H264_ENCODING=ON.
#
#   * mode 3 "no session" -- rev 1 never waited for hydrardp to authenticate,
#     then started mirror against an empty ring (the ~7MB tell).
#
#   The launchers encode all of this. Use them.
#
# USAGE (elevated, from the Hydra Shell)
#   .\test-modes.ps1 -PreflightOnly
#   .\test-modes.ps1 -Mode 2
#   .\test-modes.ps1 -Mode 3 -Gfx RFX
#
# Results -> test-results-mode<N>-<stamp>.md. Commit them.

param(
    [ValidateSet(0,1,2,3)][int]$Mode = 0,
    [switch]$PreflightOnly,
    [ValidateSet('RFX','progressive','none')][string]$Gfx = 'RFX',
    [string]$Root = 'C:\Programs\hydra'
)

$ErrorActionPreference = 'Continue'
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$results = @()
$fail    = 0

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

function Check($name, $ok, $detail = '') {
    $script:results += [PSCustomObject]@{ Test = $name; Result = $(if ($ok) {'PASS'} else {'FAIL'}); Detail = $detail }
    if ($ok) { Say "  PASS  $name" Green } else { Say "  FAIL  $name  $detail" Red; $script:fail++ }
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
    if (& query session | Select-String 'teacher') {
        Say "  NOTE  a teacher session remains -- logoff it before the next mode" Yellow
    }
}

# Wait for the seat session rather than a blind sleep. Rev 1's biggest
# structural mistake: everything downstream depends on this and it is not
# instant -- a password has to be typed.
function WaitSeat($timeoutSec = 90) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeoutSec) {
        if ((& query session | Out-String) -match 'teacher\s+\d+\s+Active') { return $true }
        Start-Sleep 2
    }
    return $false
}

Set-Location $Root

# =====================================================================
Say ""
Say "=== preflight ===" Cyan

$svc = & sc.exe qc TermService | Out-String
Check 'TermService type= own' ($svc -match 'WIN32_OWN_PROCESS') 'fix: sc config TermService type= own, then reboot'

$dll = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters' ServiceDll -EA SilentlyContinue).ServiceDll
Check 'ServiceDll = rdpwrap.dll' ($dll -like '*rdpwrap*') "currently: $dll"

$tsPid = (Get-CimInstance Win32_Service -Filter "Name='TermService'").ProcessId
$mods  = if ($tsPid) { (Get-Process -Id $tsPid -Module -EA SilentlyContinue).ModuleName } else { @() }
Check 'rdpwrap loaded into TermService' ($mods -contains 'rdpwrap.dll')

$deny = (Get-ItemProperty 'HKLM:\System\CurrentControlSet\Control\Terminal Server' fDenyTSConnections -EA SilentlyContinue).fDenyTSConnections
Check 'fDenyTSConnections = 0' ($deny -eq 0) "currently: $deny"

Check 'teacher account enabled' ([bool](Get-LocalUser teacher -EA SilentlyContinue | Where-Object Enabled))

$kf = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E96B-E325-11CE-BFC1-08002BE10318}' UpperFilters -EA SilentlyContinue).UpperFilters
$mf = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E96F-E325-11CE-BFC1-08002BE10318}' UpperFilters -EA SilentlyContinue).UpperFilters
Check 'Interception kbd filter'   ($kf -contains 'keyboard') "= $($kf -join ',')"
Check 'Interception mouse filter' ($mf -contains 'mouse')    "= $($mf -join ',')"
Check 'Interception drivers running' (((Get-Service keyboard, mouse -EA SilentlyContinue).Status | Where-Object { $_ -eq 'Running' }).Count -eq 2)

$ep  = & "$Root\dist\route_endpoint.exe" --list 2>&1 | Out-String
$cfg = Get-Content "$Root\seats.toml" -Raw -EA SilentlyContinue
if ($cfg -match '(?m)^audio_bridge\s*=\s*"([^"]+)"') {
    $want = $Matches[1]
    Check "audio_bridge id '$want' resolves" ($ep -match [regex]::Escape($want)) 'stale GUID -- a reset reissues them. route_endpoint.exe --list'
} else { Check 'audio_bridge configured' $false 'no audio_bridge line in seats.toml' }

if ($cfg -match '(?m)^display_mode\s*=\s*"([^"]+)"') { $dm = $Matches[1] } else { $dm = '(unset)' }
Say "  NOTE  seats.toml display_mode = $dm   (capture for mode 2, client for mode 3)" DarkGray
$results += [PSCustomObject]@{ Test='display_mode'; Result='INFO'; Detail=$dm }

Add-Type -AssemblyName System.Windows.Forms -EA SilentlyContinue
$screens = [System.Windows.Forms.Screen]::AllScreens
Check 'two displays present' ($screens.Count -ge 2) "found $($screens.Count)"
$screens | ForEach-Object { Say ("        {0}  {1}x{2} at ({3},{4}){5}" -f $_.DeviceName,$_.Bounds.Width,$_.Bounds.Height,$_.Bounds.X,$_.Bounds.Y,$(if($_.Primary){' primary'}else{''})) DarkGray }
$d2 = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1

Say ""
if ($fail) { Say "$fail precondition(s) failed." Red } else { Say "preconditions OK" Green }
if ($PreflightOnly) { $results | Format-Table -AutoSize; return }

if ($Mode -eq 0) { Say ""; Say "1 = mstsc   2 = sdl-freerdp   3 = hydrardp" Cyan; $Mode = [int](Read-Host 'mode') }

# =====================================================================
Say ""
Say "=== mode $Mode ===" Cyan
StopAll

switch ($Mode) {

  1 {
        Say "MODE 1 -- mstsc via hydra-start.ps1. Launcher owns the .rdp settings." Cyan
        Say "Known limitation: cursor_overlay loses z-order to the Start menu." Yellow
        Start-Process powershell -ArgumentList '-NoExit','-Command',"cd '$Root'; .\hydra-start.ps1 -Client mstsc"
        Say "  waiting for the teacher session (log in in the new window) ..." Yellow
        Check 'teacher session Active' (WaitSeat)
    }

  2 {
        Say "MODE 2 -- sdl-freerdp via hydra-start.ps1." Cyan
        Say "The launcher passes /sound, which is what creates the seat's audio" Yellow
        Say "endpoint. Without it abcap fails 0x80070490 and there is no sound." Yellow
        Start-Process powershell -ArgumentList '-NoExit','-Command',"cd '$Root'; .\hydra-start.ps1"
        Say "  waiting for the teacher session (log in in the new window) ..." Yellow
        Check 'teacher session Active' (WaitSeat)
    }

  3 {
        Say "MODE 3 -- hydrardp headless via hydra-view.ps1." Cyan
        if ($Gfx -eq 'none') {
            Say "  Gfx=none: NO CODEC. Plain bitmap updates -- video WILL look rough." Yellow
        } else {
            Say "  HYDRA_GFX=$Gfx   (never 1 -- server picks H.264 and this build crashes)" Yellow
        }
        Get-Process session_capture -EA SilentlyContinue | Stop-Process -Force
        $set = if ($Gfx -eq 'none') { 'Remove-Item Env:HYDRA_GFX -EA SilentlyContinue' } else { "`$env:HYDRA_GFX='$Gfx'" }
        Start-Process powershell -ArgumentList '-NoExit','-Command',"cd '$Root'; $set; .\hydra-view.ps1"
        Say "  waiting for the teacher session (enter the password in the new window) ..." Yellow
        Check 'teacher session Active' (WaitSeat 120)
        Check 'hydrardp running' ([bool](Get-Process hydrardp -EA SilentlyContinue))
    }
}

# --- helpers promote once the session exists -------------------------
Start-Sleep 6
$st = & "$Root\dist\hydractl.exe" status 2>&1 | Out-String
Say $st DarkGray
Check 'router running'  ($st -match 'router:\s+running')
Check 'agent:B running' ($st -match 'agent:B:\s+running') 'waiting = no seat session'
Check 'abcap:B running' ($st -match 'abcap:B:\s+running') 'waiting = no audio endpoint in the seat -- is /sound being passed?'
if ($Mode -eq 2) { Check 'capture:B running' ($st -match 'capture:B:\s+running') 'needs display_mode=capture' }

# --- mirror, only if the launcher did not already start one ----------
if (-not (Get-Process mirror -EA SilentlyContinue)) {
    Start-Process "$Root\dist\mirror.exe" -ArgumentList 'B', $d2.DeviceName
    Start-Sleep 5
}
$mm = Get-Process mirror -EA SilentlyContinue | Select-Object -First 1
Check 'mirror running' ([bool]$mm)
if ($mm) { Check 'mirror has frames (not ~7MB)' ($mm.WorkingSet64 -gt 30MB) ("{0:N0} MB" -f ($mm.WorkingSet64/1MB)) }

# --- ring liveness ---------------------------------------------------
$shm1 = & "$Root\hydra-shm.ps1" 2>&1 | Out-String
Start-Sleep 4
$shm2 = & "$Root\hydra-shm.ps1" 2>&1 | Out-String
$s1 = if ($shm1 -match 'seq=(\d+)') { [int]$Matches[1] } else { -1 }
$s2 = if ($shm2 -match 'seq=(\d+)') { [int]$Matches[1] } else { -1 }
Check 'pixel ring seq advancing' ($s1 -ge 0 -and $s2 -gt $s1) "seq $s1 -> $s2"
$c1 = if ($shm1 -match 'curSeq=(\d+)') { [int]$Matches[1] } else { -1 }
$c2 = if ($shm2 -match 'curSeq=(\d+)') { [int]$Matches[1] } else { -1 }
Check 'cursor seq advancing' ($c1 -ge 0 -and $c2 -gt $c1) "curSeq $c1 -> $c2  (frozen = agent:B stalled; check agent_B.log for err 5)"

# --- the human half --------------------------------------------------
Human "mode${Mode}: picture on panel"    'Seat desktop on the external monitor?'
Human "mode${Mode}: video smooth"        'Drag a window in the seat. Smooth?  (n = torn / blocky / stuttering)'
Human "mode${Mode}: seat cursor visible" 'Move the WIRELESS mouse. Cursor visible and tracking without stutter?'
Human "mode${Mode}: keyboard isolated"   'WIRELESS keyboard types into seat 2 ONLY?'
Human "mode${Mode}: console unaffected"  'Wired devices still drive the console?'
Human "mode${Mode}: audio at monitor"    'Play something in the seat. Sound from the MONITOR?'
if ($Mode -eq 2) { Human 'mode2: no freeze when covered' 'Cover the client thumbnail ~15s. Panel keeps updating?' }

# =====================================================================
Say ""
Say "=== result ===" Cyan
$results | Format-Table -AutoSize

$out = "$Root\test-results-mode$Mode-$stamp.md"
$l  = @("# Hydra mode $Mode acceptance test -- $(Get-Date -Format 'yyyy-MM-dd HH:mm')", '')
$l += "display_mode = $dm" + $(if ($Mode -eq 3) { "   HYDRA_GFX = $Gfx" } else { '' })
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
Say "  then: logoff <teacher session id>"
