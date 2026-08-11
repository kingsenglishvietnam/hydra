#requires -Version 5.1
<#
    fix-provider-log.ps1 -- find out which methods termsrv actually calls.

    THE QUESTION

    The provider creates and logs into a session reliably, the remote IDD is
    built, signed and staged as oem84.inf, and both sides agree on the hardware
    id HydraSeat_RemoteIDD_v1. But no display device ever appears, and the
    System event log says nothing at all -- so the RD stack is not failing to
    load the driver, it is never asking for it.

    The docs do not settle why:
      - EnableWddmIdd is termsrv TELLING the provider which mode it is in
        ("Termsrv uses this method to tell protocol stack which mode it is
        operating"), with Enabled as an [in] flag. Returning S_OK just
        acknowledges. The sample's comment implying the return value selects
        IDD vs XDDM is misleading.
      - GetHardwareId is the stack retrieving the id from the provider.

    Neither says when they are called, or whether they are called at all.

    So stop reading and instrument. This is the same move that found the /gfx
    root cause this morning: the one time something was measured rather than
    hypothesised, it produced an answer immediately.

    WHAT THIS ADDS

    A line to C:\ProgramData\Hydra\provider.log on entry to every method that
    matters for the display path, plus the lifecycle ones around it so the
    ordering is visible:

        AcceptConnection, GetClientData, AuthenticateClientToSession,
        NotifySessionId, GetInputHandles, GetVideoHandle, ConnectNotify,
        LogonNotify, NotifyCommandProcessCreated,
        GetHardwareId, OnDriverLoad, OnDriverUnload, EnableWddmIdd

    Reading the result:
      - EnableWddmIdd absent          -> termsrv never entered WDDM IDD mode
      - EnableWddmIdd(0)              -> termsrv is in XDDM mode, not IDD
      - EnableWddmIdd(1), no GetHardwareId
                                      -> it knows about IDD but is not asking
                                         this connection for a driver
      - GetHardwareId, no OnDriverLoad
                                      -> the id was taken and matched nothing,
                                         or the load failed silently
      - OnDriverLoad present          -> the devnode exists and the problem is
                                         further in

    The log is opened append-per-call and closed immediately, so a crash cannot
    lose the last line -- which is the only line that ever matters.

    -Revert restores the newest backup.
#>
[CmdletBinding()]
param(
    [string] $Source = 'C:\Programs\rdsprov\Sample\TestProtocol_Ext\WRdsProtocolConnection.cpp',
    [switch] $Build,
    [switch] $Revert
)

$ErrorActionPreference = 'Stop'

if ($Revert) {
    $bak = Get-ChildItem "$Source.bak-*" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $bak) { throw "no backup found next to $Source" }
    Copy-Item $bak.FullName $Source -Force
    Write-Host "restored $($bak.Name)" -ForegroundColor Green
    return
}

if (-not (Test-Path $Source)) { throw "not found: $Source" }
$t = [System.IO.File]::ReadAllText($Source)

if ($t -match 'HydraProvLog') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

$nl = "`r`n"

# --- the logger, inserted after the include -------------------------------
$incAnchor = '#include "WRdsProtocolConnection.h"'
if (([regex]::Matches($t, [regex]::Escape($incAnchor))).Count -ne 1) {
    throw "include anchor not found exactly once. Source has drifted."
}

$logger = @(
    '#include "WRdsProtocolConnection.h"'
    '#include <stdio.h>'
    '#include <time.h>'
    ''
    '/* Which methods does termsrv actually call?'
    ' *'
    ' * The provider creates sessions fine but no display device ever appears,'
    ' * and the System event log is silent -- so the stack is not failing to'
    ' * load the driver, it is never asking. The docs do not say when'
    ' * GetHardwareId or EnableWddmIdd are invoked, so measure it.'
    ' *'
    ' * Opened and closed per call: a crash must not be able to lose the last'
    ' * line, which is always the interesting one. */'
    'static void HydraProvLog(const char* fmt, ...)'
    '{'
    '    FILE* f = NULL;'
    '    if (fopen_s(&f, "C:\\\\ProgramData\\\\Hydra\\\\provider.log", "a") != 0 || !f)'
    '        return;'
    ''
    '    SYSTEMTIME st;'
    '    GetLocalTime(&st);'
    '    fprintf(f, "%02u:%02u:%02u.%03u [tid %5lu] ",'
    '            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,'
    '            GetCurrentThreadId());'
    ''
    '    va_list ap;'
    '    va_start(ap, fmt);'
    '    vfprintf(f, fmt, ap);'
    '    va_end(ap);'
    ''
    '    fprintf(f, "\\n");'
    '    fclose(f);'
    '}'
) -join $nl

$t = $t.Replace($incAnchor, $logger)

# --- one log line at the top of each method of interest -------------------
# Each entry: the method signature line, and what to log.
$targets = @(
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::AcceptConnection(void)';
       log = 'HydraProvLog("AcceptConnection");' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::GetClientData(WRDS_CLIENT_DATA * pClientData)';
       log = 'HydraProvLog("GetClientData");' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::AuthenticateClientToSession(WRDS_SESSION_ID * SessionId)';
       log = 'HydraProvLog("AuthenticateClientToSession");' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::NotifySessionId(WRDS_SESSION_ID * SessionId, HANDLE_PTR SessionHandle)';
       log = 'HydraProvLog("NotifySessionId");' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::GetInputHandles(HANDLE_PTR * pKeyboardHandle, HANDLE_PTR * pMouseHandle, HANDLE_PTR * pBeepHandle)';
       log = 'HydraProvLog("GetInputHandles   <-- the input path");' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::GetVideoHandle(HANDLE_PTR * pVideoHandle)';
       log = 'HydraProvLog("GetVideoHandle    <-- XDDM path, should NOT fire under IDD");' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::ConnectNotify(ULONG SessionId)';
       log = 'HydraProvLog("ConnectNotify session=%lu  (IDD creation starts here)", SessionId);' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::NotifyCommandProcessCreated(ULONG SessionId)';
       log = 'HydraProvLog("NotifyCommandProcessCreated session=%lu", SessionId);' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::GetHardwareId(WCHAR * pDisplayDriverHardwareId, DWORD Count)';
       log = 'HydraProvLog("GetHardwareId     *** THE STACK IS ASKING FOR THE DRIVER *** count=%lu", Count);' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::OnDriverLoad(ULONG SessionId, HANDLE_PTR DriverHandle)';
       log = 'HydraProvLog("OnDriverLoad      *** THE DRIVER LOADED *** session=%lu handle=%p", SessionId, (void*)DriverHandle);' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::OnDriverUnload(ULONG SessionId)';
       log = 'HydraProvLog("OnDriverUnload session=%lu", SessionId);' }
    @{ sig = 'HRESULT __stdcall CWRdsProtocolConnection::EnableWddmIdd(BOOL Enabled)';
       log = 'HydraProvLog("EnableWddmIdd(%d)  <-- termsrv telling us the mode", Enabled);' }
)

$hit = 0
$miss = @()
foreach ($x in $targets) {
    $anchor = $x.sig + $nl + '{'
    if ($t.Contains($anchor)) {
        $t = $t.Replace($anchor, $x.sig + $nl + '{' + $nl + '    ' + $x.log)
        $hit++
    } else {
        $miss += ($x.sig -replace '.*::','')
    }
}

Write-Host ("instrumented {0} of {1} methods" -f $hit, $targets.Count) -ForegroundColor Cyan
if ($miss.Count) {
    Write-Host "  not matched (signature differs):" -ForegroundColor Yellow
    $miss | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
}
if ($hit -eq 0) { throw "nothing matched -- read the source before patching." }

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green

if ($Build) {
    Write-Host ""
    Write-Host "unregistering first -- a registered provider locks its own DLL" -ForegroundColor DarkGray
    Push-Location 'C:\Programs\hydra'
    & '.\rdsprov-register.ps1' -Unregister -Apply | Out-Null
    Pop-Location

    Push-Location 'C:\Programs\rdsprov\Sample'
    & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
        TestProtocol_Ext.sln /p:Configuration=Release /p:Platform=x64 `
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.28000.0 /v:minimal
    Pop-Location

    Write-Host ""
    Write-Host "then, in order:" -ForegroundColor Cyan
    Write-Host "  1. .\rdsprov-register.ps1 -Register -Apply"
    Write-Host "  2. set Username/Password/Domain under the listener key (it is recreated empty)"
    Write-Host "  3. Remove-Item C:\ProgramData\Hydra\provider.log -ErrorAction SilentlyContinue"
    Write-Host "  4. restart TermService, then touch C:\TestProtocol\createconnection.txt"
    Write-Host "  5. Get-Content C:\ProgramData\Hydra\provider.log"
    Write-Host ""
    Write-Host "undo:  .\fix-provider-log.ps1 -Revert" -ForegroundColor DarkGray
}
