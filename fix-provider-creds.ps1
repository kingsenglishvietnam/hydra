#requires -Version 5.1
<#
    fix-provider-creds.ps1 -- read credentials from the listener key instead of
                              hardcoding them in the source.

    The sample hardcodes testuser / DontUseThis1 at the top of WaitToConnect,
    with the comment "These need to be changed to real values for your system!"

    Do not put a real password there. That tree is a clone of a public Microsoft
    repo and hydra's own repo is pushed to GitHub.

    Instead, read Username / Password / Domain from:

        HKLM\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto

    which is the same pattern termsrv's own protocol uses -- the RDP-Tcp listener
    key already carries Username, Password and Domain value entries.

    Registry ACLs there are SYSTEM/Administrators by default, and the values are
    never written to the source tree, so nothing secret reaches git.

    This is still a plaintext password at rest. The sample's own comment says to
    prefer Kerberos or another modern mechanism. For a single local teaching
    account that is probably an acceptable trade, but it should be a decision
    rather than a default.

    Set the values with (password is not echoed and is not stored in history):

        $k = 'HKLM:\System\CurrentControlSet\Control\Terminal Server\WinStations\HydraProto'
        $p = Read-Host 'password for teacher' -AsSecureString
        $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($p)
        Set-ItemProperty $k -Name Username -Value 'teacher'
        Set-ItemProperty $k -Name Domain   -Value ''
        Set-ItemProperty $k -Name Password -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($b))
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b)

    (the listener key must exist first -- register before setting these)

    -Revert restores the newest backup.
#>
[CmdletBinding()]
param(
    [string] $Source   = 'C:\Programs\rdsprov\Sample\TestProtocol_Ext\TestProtocolAPI.cpp',
    [string] $Listener = 'HydraProto',
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

if ($t -match 'HydraReadCred') {
    Write-Host "already patched -- nothing to do." -ForegroundColor Yellow
    return
}

function New-AnchorPattern([string[]] $lines) {
    ($lines | ForEach-Object { [regex]::Escape($_) }) -join '\r?\n'
}
$nl = "`r`n"

# --- anchor 1: the include, for the helper --------------------------------
$incLines = @('#include "TestProtocolAPI.h"')
$incPat   = New-AnchorPattern $incLines

# --- anchor 2: the three hardcoded credential lines -----------------------
$credLines = @(
    '    //These need to be changed to real values for your system!'
    '    swprintf_s(newConnectionConfig.UserName, MAX_STR_SIZE, L"testuser");'
    '    swprintf_s(newConnectionConfig.Password, MAX_STR_SIZE, L"DontUseThis1");'
    '    swprintf_s(newConnectionConfig.Domain, MAX_STR_SIZE, L"");'
)
$credPat = New-AnchorPattern $credLines

$mInc  = [regex]::Matches($t, $incPat)
$mCred = [regex]::Matches($t, $credPat)

Write-Host "anchor checks:" -ForegroundColor Cyan
Write-Host ("  include line        : {0}" -f $mInc.Count)
Write-Host ("  hardcoded creds     : {0}" -f $mCred.Count)
if ($mInc.Count -ne 1 -or $mCred.Count -ne 1) {
    throw "expected exactly one of each. Source has drifted -- read it before patching."
}

$keyPath = "System\\\\CurrentControlSet\\\\Control\\\\Terminal Server\\\\WinStations\\\\$Listener"

$incNew = @(
    '#include "TestProtocolAPI.h"'
    ''
    '/* Credentials come from the listener''s own registry key, not from source.'
    ' *'
    ' * The sample hardcoded testuser / DontUseThis1 here. A real password in this'
    ' * file would end up in git -- this tree is a clone of a public repo. The'
    ' * RDP-Tcp listener key already carries Username / Password / Domain value'
    ' * entries, so this is the pattern termsrv''s own protocol uses.'
    ' *'
    ' * Still plaintext at rest, protected only by the key''s ACL. Deliberate'
    ' * trade for a single local teaching account; Kerberos would be the correct'
    ' * answer for anything larger. */'
    'static const WCHAR* HYDRA_LISTENER_KEY ='
    "    L`"$keyPath`";"
    ''
    'static BOOL HydraReadCred(LPCWSTR valueName, WCHAR* out, DWORD cchOut)'
    '{'
    '    DWORD cb = cchOut * (DWORD)sizeof(WCHAR);'
    '    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, HYDRA_LISTENER_KEY, valueName,'
    '                              RRF_RT_REG_SZ, NULL, out, &cb);'
    '    if (st != ERROR_SUCCESS) { out[0] = 0; return FALSE; }'
    '    return TRUE;'
    '}'
) -join $nl

$credNew = @(
    '    /* Read from the listener key. Refuse to connect rather than falling back'
    '     * to anything hardcoded -- a silent default would be worse than a failure. */'
    '    if (!HydraReadCred(L"Username", newConnectionConfig.UserName, MAX_STR_SIZE) ||'
    '        !HydraReadCred(L"Password", newConnectionConfig.Password, MAX_STR_SIZE))'
    '    {'
    '        OutputDebugStringW(L"[hydraproto] no Username/Password under the listener key"'
    '                           L" -- refusing to create a connection\n");'
    '        return FALSE;'
    '    }'
    '    if (!HydraReadCred(L"Domain", newConnectionConfig.Domain, MAX_STR_SIZE))'
    '        newConnectionConfig.Domain[0] = 0;'
) -join $nl

$t = [regex]::Replace($t, $incPat,  { $incNew },  1)
$t = [regex]::Replace($t, $credPat, { $credNew }, 1)

# Wipe the config before the thread returns. The sample's own comment asks for
# this and then does not do it.
$retLines = @(
    '    CreateTestConnection(pListenerCallback, &newConnectionConfig, &newConnectionOutput);'
    '    return TRUE;'
)
$retPat = New-AnchorPattern $retLines
if (([regex]::Matches($t, $retPat)).Count -eq 1) {
    $retNew = @(
        '    CreateTestConnection(pListenerCallback, &newConnectionConfig, &newConnectionOutput);'
        ''
        '    /* Do not leave the password sitting on this thread''s stack. */'
        '    SecureZeroMemory(&newConnectionConfig, sizeof(newConnectionConfig));'
        '    return TRUE;'
    ) -join $nl
    $t = [regex]::Replace($t, $retPat, { $retNew }, 1)
    Write-Host "  (also added SecureZeroMemory of the config)" -ForegroundColor DarkGray
} else {
    Write-Host "  (could not add SecureZeroMemory -- return block not matched, not fatal)" -ForegroundColor Yellow
}

$bak = "$Source.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $Source $bak
[System.IO.File]::WriteAllText($Source, $t)

Write-Host ""
Write-Host "patched. backup: $bak" -ForegroundColor Green
Write-Host ""
Select-String -Path $Source -Pattern 'HydraReadCred|HYDRA_LISTENER_KEY|SecureZeroMemory' |
    Select-Object LineNumber, Line | Format-Table -AutoSize

if ($Build) {
    Write-Host ""
    Push-Location 'C:\Programs\rdsprov\Sample'
    & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
        TestProtocol_Ext.sln /p:Configuration=Release /p:Platform=x64 `
        /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.28000.0 /v:minimal
    Pop-Location
    Write-Host ""
    Write-Host "then, in order:" -ForegroundColor Cyan
    Write-Host "  1. .\rdsprov-register.ps1 -Register -Apply     (creates the listener key)"
    Write-Host "  2. set Username / Password / Domain under it   (see the header of this script)"
    Write-Host "  3. touch C:\TestProtocol\createconnection.txt"
    Write-Host "  4. query session -- watch for hydraproto#0 reaching Active and STAYING"
    Write-Host ""
    Write-Host "Active and stable = a Windows session created AND logged in by our own" -ForegroundColor DarkGray
    Write-Host "code, with no RDP anywhere in the path." -ForegroundColor DarkGray
    Write-Host "undo:  .\fix-provider-creds.ps1 -Revert" -ForegroundColor DarkGray
}
