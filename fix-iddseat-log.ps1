#requires -Version 5.1
<#
    fix-iddseat-log.ps1 -- name the call that fails.

    WHERE WE ARE

    The ETW trace settled everything up to the driver:

        Kernel-PnP   Device ... was configured. oem84.inf 1.0.0.4, Status: 0
        Kernel-PnP   Begin device add operation for driver \Driver\WudfRd
        UMDF         Host Process has SUCCESSFULLY LOADED drivers
        UMDF         The UMDF Host is loading driver iddseatremote at level 0
        UMDF         The UMDF Host failed to load the driver at level 0.
                     The error reported was 3489660941
        Kernel-PnP   New problem code: 31, status 0xC0000001

    3489660941 == 0xD000000D == STATUS_INVALID_PARAMETER.

    So packaging, policy, signing and installation are all DONE. The driver is
    loaded into WUDFHost and its own entry path returns invalid-parameter.

    Three calls in IddSeatDeviceAdd can produce that, and each returns early,
    so from outside they are indistinguishable:

        IddCxDeviceInitConfig   (~line 415)
        WdfDeviceCreate         (~line 427)
        IddCxDeviceInitialize   (~line 446)

    ReadSeatProperties is NOT a candidate -- it defaults seat to L"B" and mode
    to L"1920x1080@60" before querying, so absent properties are handled.

    WHAT THIS ADDS

    A file log at C:\ProgramData\Hydra\iddseat.log, written on entry to
    DriverEntry and DeviceAdd and after each of the three calls with its
    NTSTATUS in hex. Same approach that worked on the provider: stop guessing,
    make the code say which line.

    A file rather than OutputDebugStringW because WUDFHost runs as a service
    and nothing is attached to catch debug strings. The log is opened and
    closed per line so a failing load cannot lose the last one -- which is
    always the interesting one.

    The driver runs as SYSTEM in its own host process, so it can write to
    ProgramData without trouble.

    -Revert restores the newest backup.
#>
[CmdletBinding()]
param(
    [string] $Source = 'C:\Programs\hydra\iddseat\iddseat.cpp',
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

if ($t -match 'IddSeatLog') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

function New-AnchorPattern([string[]] $lines) {
    ($lines | ForEach-Object { [regex]::Escape($_) }) -join '\r?\n'
}
$nl = "`r`n"

# --- 1. the logger, in front of DriverEntry -------------------------------
$entryLines = @(
    'extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)'
    '{'
)
$entryPat = New-AnchorPattern $entryLines

$logger = @(
    '/* Which call fails?'
    ' *'
    ' * The UMDF host reports "failed to load the driver at level 0, error'
    ' * 3489660941" == 0xD000000D == STATUS_INVALID_PARAMETER, and problem code'
    ' * 31 on the devnode. Everything before this point -- match, policy,'
    ' * signature, install, host start -- succeeds. Three calls in DeviceAdd can'
    ' * return that status and each returns early, so from outside they cannot be'
    ' * told apart.'
    ' *'
    ' * A file rather than OutputDebugStringW: WUDFHost is a service and nothing'
    ' * is attached to catch debug strings. Opened and closed per line so a'
    ' * failing load cannot lose the last one. */'
    'static void IddSeatLog(const char* fmt, ...)'
    '{'
    '    FILE* f = nullptr;'
    '    if (fopen_s(&f, "C:\\\\ProgramData\\\\Hydra\\\\iddseat.log", "a") != 0 || !f)'
    '        return;'
    ''
    '    SYSTEMTIME st;'
    '    GetLocalTime(&st);'
    '    fprintf(f, "%02u:%02u:%02u.%03u [pid %5lu] ",'
    '            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,'
    '            GetCurrentProcessId());'
    ''
    '    va_list ap;'
    '    va_start(ap, fmt);'
    '    vfprintf(f, fmt, ap);'
    '    va_end(ap);'
    ''
    '    fprintf(f, "\\n");'
    '    fclose(f);'
    '}'
    ''
    'extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)'
    '{'
    '    IddSeatLog("DriverEntry");'
) -join $nl

if (([regex]::Matches($t, $entryPat)).Count -ne 1) {
    throw "DriverEntry signature not found exactly once. Read the source before patching."
}
$t = [regex]::Replace($t, $entryPat, { $logger }, 1)

# --- 2. DeviceAdd entry ---------------------------------------------------
$addLines = @(
    '    std::wstring seat, modeStr;'
    '    ReadSeatProperties(pDeviceInit, seat, modeStr);'
)
$addPat = New-AnchorPattern $addLines
$addNew = @(
    '    IddSeatLog("DeviceAdd: entered");'
    '    std::wstring seat, modeStr;'
    '    ReadSeatProperties(pDeviceInit, seat, modeStr);'
    '    IddSeatLog("DeviceAdd: seat properties read");'
) -join $nl

if (([regex]::Matches($t, $addPat)).Count -ne 1) {
    throw "DeviceAdd entry lines not found exactly once."
}
$t = [regex]::Replace($t, $addPat, { $addNew }, 1)

# --- 3. the three candidates ----------------------------------------------
$cands = @(
    @{
        Name = 'IddCxDeviceInitConfig'
        Old  = @(
            '    NTSTATUS status = IddCxDeviceInitConfig(pDeviceInit, &cfg);'
            '    if (!NT_SUCCESS(status)) return status;'
        )
        New  = @(
            '    NTSTATUS status = IddCxDeviceInitConfig(pDeviceInit, &cfg);'
            '    IddSeatLog("IddCxDeviceInitConfig -> 0x%08X", status);'
            '    if (!NT_SUCCESS(status)) return status;'
        )
    }
    @{
        Name = 'WdfDeviceCreate'
        Old  = @(
            '    status = WdfDeviceCreate(&pDeviceInit, &attr, &device);'
            '    if (!NT_SUCCESS(status)) return status;'
        )
        New  = @(
            '    status = WdfDeviceCreate(&pDeviceInit, &attr, &device);'
            '    IddSeatLog("WdfDeviceCreate -> 0x%08X", status);'
            '    if (!NT_SUCCESS(status)) return status;'
        )
    }
    @{
        Name = 'IddCxDeviceInitialize'
        Old  = @('    return IddCxDeviceInitialize(device);')
        New  = @(
            '    NTSTATUS initStatus = IddCxDeviceInitialize(device);'
            '    IddSeatLog("IddCxDeviceInitialize -> 0x%08X", initStatus);'
            '    return initStatus;'
        )
    }
)

foreach ($c in $cands) {
    $p = New-AnchorPattern $c.Old
    $n = ($c.New) -join $nl
    $m = [regex]::Matches($t, $p)
    if ($m.Count -ne 1) {
        throw ("{0} anchor matched {1} times, expected 1." -f $c.Name, $m.Count)
    }
    $t = [regex]::Replace($t, $p, { $n }, 1)
    Write-Host ("  instrumented {0}" -f $c.Name) -ForegroundColor DarkGray
}

# --- 4. D0Entry, where IddCxAdapterInitAsync lives ------------------------
$d0Lines = @(
    '    IDARG_OUT_ADAPTER_INIT out{};'
    '    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);'
)
$d0Pat = New-AnchorPattern $d0Lines
if (([regex]::Matches($t, $d0Pat)).Count -eq 1) {
    $d0New = @(
        '    IDARG_OUT_ADAPTER_INIT out{};'
        '    IddSeatLog("D0Entry: calling IddCxAdapterInitAsync, caps.Flags=0x%X", (unsigned)caps.Flags);'
        '    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);'
        '    IddSeatLog("IddCxAdapterInitAsync -> 0x%08X", status);'
    ) -join $nl
    $t = [regex]::Replace($t, $d0Pat, { $d0New }, 1)
    Write-Host "  instrumented IddCxAdapterInitAsync" -ForegroundColor DarkGray
} else {
    Write-Host "  (D0Entry anchor not matched -- not fatal, DeviceAdd is the suspect)" -ForegroundColor Yellow
}

# --- 5. stdio, if the source does not already have it ---------------------
if ($t -notmatch '#include\s+<cstdio>' -and $t -notmatch '#include\s+<stdio\.h>') {
    # Put it after the first #include so it lands before any use.
    $firstInc = [regex]::Match($t, '(?m)^#include[^\r\n]*$')
    if ($firstInc.Success) {
        $t = $t.Insert($firstInc.Index + $firstInc.Length,
                       $nl + '#include <cstdio>' + $nl + '#include <cstdarg>')
        Write-Host "  added <cstdio> / <cstdarg>" -ForegroundColor DarkGray
    }
}

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'IddSeatLog' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    & 'C:\Programs\hydra\build-driver.ps1' -Remote
    Write-Host ""
    Write-Host "then, in order:" -ForegroundColor Cyan
    Write-Host "  stampinf -f dist\driver-remote\iddseat-remote.inf -d * -a amd64 -v 1.0.0.5 -u 2.35.0"
    Write-Host "  Inf2Cat /driver:dist\driver-remote /os:10_x64"
    Write-Host "  .\sign-driver.ps1 -DriverDir .\dist\driver-remote"
    Write-Host "  pnputil /delete-driver oem84.inf /uninstall"
    Write-Host "  pnputil /add-driver dist\driver-remote\iddseat-remote.inf /install"
    Write-Host "  Remove-Item C:\ProgramData\Hydra\iddseat.log -ErrorAction SilentlyContinue"
    Write-Host "  ... trigger ..."
    Write-Host "  Get-Content C:\ProgramData\Hydra\iddseat.log"
    Write-Host ""
    Write-Host "the last line before the log stops is the call that returns 0xD000000D." -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-iddseat-log.ps1 -Revert" -ForegroundColor DarkGray
}
