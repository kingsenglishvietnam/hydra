#Requires -RunAsAdministrator
<#
.SYNOPSIS
    hydra-termsrv-guard.ps1 - re-applies the concurrent-session patch to termsrv.dll
    whenever Windows Update replaces it.

.DESCRIPTION
    - Hashes termsrv.dll and compares against the last hash this script produced.
    - If changed: searches a pattern table, patches only on a single unambiguous match.
    - Archives every original it sees (never overwrites an archive file).
    - Unknown build -> touches nothing, logs ERROR, drops state\termsrv-UNPATCHED flag, exit 2.

.EXAMPLE
    .\hydra-termsrv-guard.ps1 -DryRun
    .\hydra-termsrv-guard.ps1
    .\hydra-termsrv-guard.ps1 -Install
    .\hydra-termsrv-guard.ps1 -Restore
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Install,
    [switch]$Uninstall,
    [switch]$Restore,
    [switch]$Force,
    [string]$Root
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

# Repo root: two levels up when run from tools\termsrv-guard (where hydra7.ps1 lives);
# otherwise the historical default. Override with -Root.
if (-not $Root) {
    $up2  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    $Root = if (Test-Path (Join-Path $up2 'hydra7.ps1')) { $up2 } else { 'C:\Programs\hydra' }
}
$Dll      = "$env:SystemRoot\System32\termsrv.dll"
$Archive  = Join-Path $Root 'termsrv-archive'
$StateDir = Join-Path $Root 'state'
$StateF   = Join-Path $StateDir 'termsrv-guard.json'
$FlagF    = Join-Path $StateDir 'termsrv-UNPATCHED'
$LogF     = Join-Path $Root 'logs\termsrv-guard.log'
$TaskName = 'Hydra termsrv guard'

# Pattern table. '??' = wildcard byte. Newest first. Find and Repl must be equal length.
$Patterns = @(
    @{ Name = '26100.8521+'; Find = '8B 8F 38 06 00 00 45 3B C1 75';            Repl = '8B 8F 38 06 00 00 45 3B C1 EB' }
    @{ Name = '24H2';        Find = '8B 81 38 06 00 00 39 81 3C 06 00 00 75';   Repl = 'B8 00 01 00 00 89 81 38 06 00 00 90 EB' }
    @{ Name = 'legacy';      Find = '39 81 3C 06 00 00 0F 84 ?? ?? ?? ??';      Repl = 'B8 00 01 00 00 89 81 38 06 00 00 90' }
)

# ---------------------------------------------------------------- helpers

function Write-Log([string]$Msg, [string]$Lvl = 'INFO') {
    $line = '{0:yyyy-MM-dd HH:mm:ss} [{1}] {2}' -f (Get-Date), $Lvl, $Msg
    New-Item -ItemType Directory -Force -Path (Split-Path $LogF) | Out-Null
    Add-Content -Path $LogF -Value $line
    Write-Host $line
}

function ConvertTo-HexRegex([string]$Pattern) {
    ($Pattern.Trim() -split '\s+' | ForEach-Object {
        if ($_ -eq '??') { '[0-9A-F]{2}' } else { $_.ToUpper() }
    }) -join '-'
}

function Find-Pattern([string]$Hex, [string]$Pattern) {
    # Hex is BitConverter output "AA-BB-CC"; first token is literal so matches stay byte-aligned.
    @([regex]::Matches($Hex, (ConvertTo-HexRegex $Pattern)) | ForEach-Object { [int]($_.Index / 3) })
}

function ConvertTo-Bytes([string]$Pattern) {
    [byte[]]($Pattern.Trim() -split '\s+' | ForEach-Object { [Convert]::ToByte($_, 16) })
}

function Get-DllInfo {
    $bytes = [IO.File]::ReadAllBytes($Dll)
    $vi    = (Get-Item $Dll).VersionInfo
    [pscustomobject]@{
        Bytes   = $bytes
        Hex     = [BitConverter]::ToString($bytes)
        Sha     = (Get-FileHash $Dll -Algorithm SHA256).Hash
        Version = '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
    }
}

function Get-RdpSessionCount {
    @(& qwinsta.exe 2>$null | Select-String 'rdp-tcp#').Count
}

function Save-State($Obj) {
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    $Obj | ConvertTo-Json | Set-Content -Path $StateF -Encoding UTF8
}

function Write-SystemDll([byte[]]$Bytes) {
    $aclSave = Join-Path $env:TEMP 'termsrv.acl'
    & icacls.exe $Dll /save $aclSave | Out-Null
    & takeown.exe /F $Dll /A | Out-Null
    & icacls.exe $Dll /grant '*S-1-5-32-544:F' | Out-Null

    Stop-Service UmRdpService -Force -ErrorAction SilentlyContinue
    Stop-Service TermService -Force
    (Get-Service TermService).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))

    try {
        $written = $false
        for ($i = 0; $i -lt 20 -and -not $written; $i++) {
            try { [IO.File]::WriteAllBytes($Dll, $Bytes); $written = $true }
            catch { Start-Sleep -Milliseconds 500 }
        }
        if (-not $written) { throw 'termsrv.dll still locked after 10s' }
    }
    finally {
        # Owner first, while Administrators still hold Full (incl. WRITE_OWNER); then put the DACL back.
        & icacls.exe $Dll /setowner 'NT SERVICE\TrustedInstaller' 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Log 'Could not return ownership to TrustedInstaller' 'WARN' }
        & icacls.exe "$env:SystemRoot\System32" /restore $aclSave 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Log 'Could not restore original ACL on termsrv.dll' 'WARN' }
        Remove-Item $aclSave -ErrorAction SilentlyContinue
        Start-Service TermService
        Start-Service UmRdpService -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- task install

if ($Install) {
    # In-box Windows PowerShell: fixed path, unlike a Store-installed pwsh whose path carries its version.
    $exe = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $action  = New-ScheduledTaskAction -Execute $exe `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $princ   = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
    Register-ScheduledTask -TaskName $TaskName `
        -Action $action `
        -Trigger $trigger `
        -Principal $princ `
        -Force | Out-Null
    Write-Log "Installed scheduled task '$TaskName' ($exe, at startup, SYSTEM)"
    exit 0
}

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Log "Removed scheduled task '$TaskName'"
    exit 0
}

# ---------------------------------------------------------------- main

$info  = Get-DllInfo
$state = if (Test-Path $StateF) { Get-Content $StateF -Raw | ConvertFrom-Json } else { $null }
Write-Log "termsrv.dll $($info.Version) sha256 $($info.Sha.Substring(0,16))..."

# Prerequisites the patch can't fix: warn only, never change them silently.
$tsKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server'
if ((Get-ItemProperty $tsKey -ErrorAction SilentlyContinue).fDenyTSConnections -eq 1) {
    Write-Log 'Remote Desktop is switched off (fDenyTSConnections=1): seats will fail with error 10061' 'WARN'
}
$svcDll = (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\TermService\Parameters).ServiceDll
if ($svcDll -notmatch '\\System32\\termsrv\.dll$') {
    Write-Log "ServiceDll is '$svcDll', not termsrv.dll (RDP Wrapper still installed?)" 'WARN'
}

if ($Restore) {
    if (-not $state -or $state.PatchedSha -ne $info.Sha) {
        Write-Log 'Restore: current dll is not the one this script patched; refusing.' 'ERROR'; exit 1
    }
    if ($DryRun) { Write-Log "DryRun: would restore $($state.OriginalFile)"; exit 0 }
    Write-SystemDll ([IO.File]::ReadAllBytes($state.OriginalFile))
    Remove-Item $StateF
    Write-Log "Restored original from $($state.OriginalFile)"
    exit 0
}

if ($state -and $state.PatchedSha -eq $info.Sha) {
    Remove-Item $FlagF -ErrorAction SilentlyContinue
    Write-Log "OK: already patched ($($state.Pattern)), nothing to do"
    exit 0
}

# Find exactly one pattern with exactly one hit
$chosen = $null; $offset = -1
foreach ($p in $Patterns) {
    $hits = Find-Pattern $info.Hex $p.Find
    Write-Log ("  pattern {0,-12} find-hits={1}" -f $p.Name, $hits.Count)
    if ($hits.Count -gt 1) {
        Write-Log "Pattern $($p.Name) is ambiguous ($($hits.Count) hits); refusing to patch" 'ERROR'
        New-Item -ItemType File -Force -Path $FlagF | Out-Null
        exit 2
    }
    if ($hits.Count -eq 1 -and -not $chosen) { $chosen = $p; $offset = $hits[0] }
}

if (-not $chosen) {
    $already = $Patterns | Where-Object { (Find-Pattern $info.Hex $_.Repl).Count -eq 1 } | Select-Object -First 1
    if ($already) {
        Write-Log "Already patched by something else (matches $($already.Name) replacement); recording state"
        Save-State ([pscustomobject]@{
            Version = $info.Version; PatchedSha = $info.Sha; Pattern = "$($already.Name) (external)"
            OriginalSha = $null; OriginalFile = $null; When = (Get-Date).ToString('s')
        })
        Remove-Item $FlagF -ErrorAction SilentlyContinue
        exit 0
    }
    Write-Log "UNKNOWN BUILD $($info.Version): no pattern matched. termsrv.dll untouched. Add a pattern." 'ERROR'
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    Set-Content -Path $FlagF -Value $info.Version
    exit 2
}

$repl = ConvertTo-Bytes $chosen.Repl
if ($repl.Length -ne ((ConvertTo-HexRegex $chosen.Find) -split '-').Count) {
    Write-Log "Pattern $($chosen.Name): Find/Repl length mismatch" 'ERROR'; exit 3
}

Write-Log ("Match: {0} at offset 0x{1:X}" -f $chosen.Name, $offset)

$sessions = Get-RdpSessionCount
if ($sessions -gt 0 -and -not $Force) {
    Write-Log "$sessions RDP session(s) active; patching restarts TermService. Re-run with -Force." 'WARN'
    exit 4
}

if ($DryRun) { Write-Log 'DryRun: would archive original and patch'; exit 0 }

# Archive original (never overwrite)
New-Item -ItemType Directory -Force -Path $Archive | Out-Null
$origFile = Join-Path $Archive ('termsrv_{0}_{1}.dll' -f $info.Version, $info.Sha.Substring(0, 8))
if (-not (Test-Path $origFile)) {
    [IO.File]::WriteAllBytes($origFile, $info.Bytes)
    Write-Log "Archived original -> $origFile"
}

$new = [byte[]]$info.Bytes.Clone()
for ($i = 0; $i -lt $repl.Length; $i++) { $new[$offset + $i] = $repl[$i] }

$sha = [Security.Cryptography.SHA256]::Create()
$expected = [BitConverter]::ToString($sha.ComputeHash($new)).Replace('-', '')

Write-SystemDll $new

$after = (Get-FileHash $Dll -Algorithm SHA256).Hash
if ($after -ne $expected) {
    Write-Log "Post-write hash mismatch (got $after); check $Dll by hand" 'ERROR'
    exit 5
}
if ((Get-Service TermService).Status -ne 'Running') {
    Write-Log 'TermService did not come back up; consider -Restore' 'ERROR'
    exit 6
}

Save-State ([pscustomobject]@{
    Version = $info.Version; PatchedSha = $after; Pattern = $chosen.Name
    OriginalSha = $info.Sha; OriginalFile = $origFile; When = (Get-Date).ToString('s')
})
Remove-Item $FlagF -ErrorAction SilentlyContinue
Write-Log "Patched OK ($($chosen.Name)); TermService running"
exit 0
