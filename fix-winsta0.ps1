# fix-winsta0.ps1 -- attach seatB_agent to WinSta0 explicitly.
#
# HYPOTHESIS BEING TESTED
#   CreateProcessAsUserW's si.lpDesktop = "winsta0\default" sets the child's
#   STARTING DESKTOP but does not guarantee its PROCESS WINDOW STATION. A SYSTEM
#   token stamped into another session via SetTokenInformation(TokenSessionId)
#   can land on a station that is not WinSta0. OpenInputDesktop(0, ...) then
#   resolves against THAT station, succeeds, SetThreadDesktop succeeds, and the
#   agent logs "re-attached and recovered" while sitting on a desktop that
#   receives nothing. Every subsequent SendInput returns ERROR_ACCESS_DENIED (5).
#
#   Consistent with the observed diagnostic: our integrity=0x4000 (System) and
#   "NO foreground window in this session" while explorer.exe, msedge.exe and a
#   live RDP client are demonstrably running in session 2.
#
# WHAT IT DOES
#   Inserts an OpenWindowStation("WinSta0") + SetProcessWindowStation() block at
#   the top of main(), immediately after disable_quickedit() and BEFORE the
#   cursor_pos_thread spawn (that thread reads cursor position, so it must not
#   start on the wrong station).
#
#   Logs the resolved station name either way. That line is the whole point:
#     "[agent] window station: WinSta0"   -> theory dead, look elsewhere
#     anything else, or the open failing  -> theory proven
#
#   Narrow fprintf via WideCharToMultiByte, NOT fwprintf -- mixing wide and
#   narrow on the same stream is UB in the Windows CRT and has already swallowed
#   one diagnostic in this codebase (see the --learn path in seat_router.c).
#
# USAGE (from an x64 Native Tools prompt in C:\Programs\hydra):
#   .\fix-winsta0.ps1
#   .\fix-winsta0.ps1 -Revert        # restore the newest .bak-* and stop

param(
    [string]$File   = 'C:\Programs\hydra\input\seatB_agent.c',
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $File)) { throw "not found: $File" }

# ---- revert ---------------------------------------------------------------
if ($Revert) {
    $bak = Get-ChildItem "$File.bak-*" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $bak) { throw "no backup found matching $File.bak-*" }
    Copy-Item $bak.FullName $File -Force
    Write-Host "reverted from $($bak.Name)" -ForegroundColor Yellow
    return
}

# ---- gate: is it already patched? ----------------------------------------
$text = [System.IO.File]::ReadAllText($File)

if ($text -match 'SetProcessWindowStation') {
    Write-Host "already patched -- SetProcessWindowStation present. Nothing to do." -ForegroundColor Yellow
    return
}

# ---- gate: anchor must exist exactly once --------------------------------
# \r?\n throughout: this file has mixed CRLF/LF from earlier patch scripts.
$anchor = '(?m)^    disable_quickedit\(\);'
$hits   = ([regex]::Matches($text, $anchor)).Count
if ($hits -ne 1) {
    throw "anchor 'disable_quickedit();' matched $hits times (expected 1) -- stopping, file not modified."
}

# ---- backup ---------------------------------------------------------------
$bak = "$File.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $File $bak -Force
Write-Host "backup: $bak" -ForegroundColor DarkGray

# ---- the insert -----------------------------------------------------------
$insert = @'
    disable_quickedit();

    /* Attach to WinSta0 explicitly before touching any desktop.
     *
     * CreateProcessAsUserW's lpDesktop sets the STARTING DESKTOP but not the
     * process WINDOW STATION. A SYSTEM process stamped into another session can
     * therefore resolve OpenInputDesktop() against the wrong station: the call
     * succeeds, SetThreadDesktop succeeds, we log "re-attached and recovered",
     * and every SendInput after it still returns ERROR_ACCESS_DENIED (5)
     * because the desktop we attached to is not the one receiving input.
     *
     * The logged station name is the diagnostic. "WinSta0" here means this was
     * never the problem; anything else means it was. */
    {
        HWINSTA ws = OpenWindowStationW(L"WinSta0", FALSE, WINSTA_ALL_ACCESS);
        if (ws && SetProcessWindowStation(ws)) {
            wchar_t nm[128];
            char    utf8[256];
            DWORD   n = 0;
            nm[0] = 0;
            if (GetUserObjectInformationW(ws, UOI_NAME, nm, sizeof(nm), &n) &&
                WideCharToMultiByte(CP_UTF8, 0, nm, -1, utf8, sizeof(utf8),
                                    NULL, NULL) > 0) {
                fprintf(stderr, "[agent] window station: %s\n", utf8);
            } else {
                fprintf(stderr, "[agent] window station: (name query failed err=%lu)\n",
                        GetLastError());
            }
        } else {
            fprintf(stderr, "[agent] OpenWindowStation(WinSta0) failed err=%lu"
                            " -- staying on inherited station\n", GetLastError());
        }
        fflush(stderr);
    }
'@ -replace "`r?`n", "`r`n"

$text = [regex]::Replace($text, $anchor, [System.Text.RegularExpressions.MatchEvaluator]{ $insert }, 1)

[System.IO.File]::WriteAllText($File, $text)

# ---- verify ---------------------------------------------------------------
$check = Select-String -Path $File -Pattern 'OpenWindowStationW|SetProcessWindowStation|window station:' |
         Select-Object LineNumber, Line

if ($check.Count -lt 3) {
    Copy-Item $bak $File -Force
    throw "verification failed ($($check.Count) hits, expected 3+) -- reverted from backup."
}

Write-Host ""
Write-Host "patched:" -ForegroundColor Green
$check | ForEach-Object { Write-Host ("  {0,5}  {1}" -f $_.LineNumber, $_.Line.Trim()) }

Write-Host ""
Write-Host "REBUILD, then redeploy and restart:" -ForegroundColor Cyan
Write-Host "  .\build.ps1"
Write-Host "  .\setup.ps1"
Write-Host "  Start-Service Hydra"
Write-Host ""
Write-Host "If build.ps1 does not cover this target, direct:" -ForegroundColor DarkGray
Write-Host '  cl /O2 /Fe:dist\seatB_agent.exe input\seatB_agent.c user32.lib ws2_32.lib winmm.lib'
Write-Host ""
Write-Host "THEN READ THE NEW LINE -- this is the whole test:" -ForegroundColor Cyan
Write-Host '  Select-String -Path C:\ProgramData\Hydra\logs\agent_B.log -Pattern ''window station'''
Write-Host ""
Write-Host "  WinSta0        -> theory dead, err 5 is something else" -ForegroundColor Yellow
Write-Host "  anything else  -> theory proven, and this is the fix" -ForegroundColor Yellow
