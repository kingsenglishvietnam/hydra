#Requires -RunAsAdministrator
<#
.SYNOPSIS
    hydra-dropcheck.ps1 v2 - who keeps resetting seat B's loopback RDP connection?

.DESCRIPTION
    Read-only. Finds every seat B drop (RDPClient event 1026) in the last -Hours, groups drops
    that land close together, and lists whatever else Windows logged around each group: System
    (Wi-Fi driver, Tcpip, Kernel-Power, display TDRs, service starts and stops), Application
    (security and VPN products), WLAN-AutoConfig, NetworkProfile and Windows Firewall.

    Also prints the exact OS build with recent updates, termsrv.dll's build and write time, every
    TermService crash of the last -CrashDays days (faulting module, and how many fall before or
    after that write), the WFP providers registered on the box with their filter counts (the raw
    XML is kept beside the report),
    and every running kernel driver not published by Microsoft - the usual places a TCP reset on
    a loopback connection comes from. Everything is mirrored to <OutRoot>\dropcheck-<stamp>.txt.

.EXAMPLE
    & C:\Scripts\windows\hydra-dropcheck.ps1

.EXAMPLE
    & C:\Scripts\windows\hydra-dropcheck.ps1 -Hours 48

.NOTES
    v1  2026-10-15  initial
    v2  2026-10-15  TermService crash records, termsrv.dll build, WFP parser fixed (root element
                    is not <wfpstate>) and now counts filters per provider
#>
[CmdletBinding()]
param(
    [int]$Hours      = 12,
    [int]$Before     = 20,
    [int]$After      = 5,
    [int]$CrashDays  = 30,
    [string]$OutRoot = 'C:\Programs\hydra-diag'
)

$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false

New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null
$outFile = Join-Path $OutRoot ('dropcheck-{0}.txt' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

function Say {
    param([string]$Text = '', [ConsoleColor]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
    Add-Content -LiteralPath $outFile -Value $Text -Encoding utf8
}

function Get-FirstLine {
    param($Text)
    ("$Text" -split '\r?\n')[0]
}

function Get-CrashFields {
    param([string]$Message)
    $f = [ordered]@{ Module = '?'; Exception = '?'; Offset = '?' }
    if ($Message -match 'Faulting module name:\s*([^,\r\n]+)') { $f.Module = $Matches[1].Trim() }
    if ($Message -match 'Exception code:\s*(\S+)')              { $f.Exception = $Matches[1] }
    if ($Message -match 'Fault offset:\s*(\S+)')                { $f.Offset = $Matches[1] }
    [pscustomobject]$f
}

function Get-WfpProviderRows {
    param([string]$Path)
    $x = New-Object System.Xml.XmlDocument
    $x.Load($Path)
    $counts = @{}
    foreach ($f in $x.SelectNodes('//filters/item')) {
        $k = "$($f.providerKey)"
        if ($k) { $counts[$k] = 1 + [int]$counts[$k] }
    }
    $rows = @(foreach ($p in $x.SelectNodes('//providers/item')) {
        [pscustomobject]@{ Name = "$($p.displayData.name)"; Filters = [int]$counts["$($p.providerKey)"] }
    })
    [pscustomobject]@{ Root = $x.DocumentElement.Name; Rows = $rows }
}

# ------------------------------------------------------------------ build and updates

$cv = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
Say "hydra-dropcheck v2  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  ->  $outFile" 'Cyan'
Say "  Windows $($cv.DisplayVersion)  build $($cv.CurrentBuild).$($cv.UBR)"
Get-HotFix | Sort-Object InstalledOn -Descending | Select-Object -First 6 | ForEach-Object {
    Say ('  {0,-11} {1,-17} {2:yyyy-MM-dd}' -f $_.HotFixID, $_.Description, $_.InstalledOn)
}

$tdll = Get-Item -LiteralPath "$env:SystemRoot\System32\termsrv.dll" -ErrorAction SilentlyContinue
if ($tdll) {
    $vi = $tdll.VersionInfo
    Say ('  termsrv.dll {0}.{1}.{2}.{3}  written {4:yyyy-MM-dd HH:mm}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart, $tdll.LastWriteTime)
}

# ------------------------------------------------------------------ drops and neighbours

$since = (Get-Date).AddHours(-$Hours)
$drops = @(Get-WinEvent -ErrorAction SilentlyContinue -FilterHashtable @{
        LogName   = 'Microsoft-Windows-TerminalServices-RDPClient/Operational'
        Id        = 1026
        StartTime = $since } | Sort-Object TimeCreated)

Say ''
Say "== seat B drops in the last $Hours h (RDPClient 1026): $($drops.Count)" 'Cyan'

# Group drops whose windows overlap, so a burst prints once.
$groups = @()
foreach ($d in $drops) {
    $from = $d.TimeCreated.AddSeconds(-$Before)
    $to   = $d.TimeCreated.AddSeconds($After)
    if ($groups.Count -and $from -le $groups[-1].To) {
        $groups[-1].To     = $to
        $groups[-1].Drops += $d
    } else {
        $groups += [pscustomobject]@{ From = $from; To = $to; Drops = @($d) }
    }
}

$logs = 'System', 'Application',
        'Microsoft-Windows-WLAN-AutoConfig/Operational',
        'Microsoft-Windows-NetworkProfile/Operational',
        'Microsoft-Windows-Windows Firewall With Advanced Security/Firewall'

foreach ($g in $groups) {
    $near = @(foreach ($l in $logs) {
        Get-WinEvent -ErrorAction SilentlyContinue -FilterHashtable @{ LogName = $l; StartTime = $g.From; EndTime = $g.To }
    })
    Say ''
    Say ('-- {0:HH:mm:ss} to {1:HH:mm:ss}  ({2} drop(s))' -f $g.From, $g.To, $g.Drops.Count) 'Yellow'
    foreach ($e in @(@($near) + @($g.Drops) | Sort-Object TimeCreated)) {
        if ($e.LogName -like '*RDPClient*') {
            Say ('   {0:HH:mm:ss}  << DROP  {1}' -f $e.TimeCreated, (Get-FirstLine $e.Message)) 'Yellow'
        } else {
            Say ('   {0:HH:mm:ss}  {1,-44} {2,5}  {3}' -f $e.TimeCreated, $e.ProviderName, $e.Id, (Get-FirstLine $e.Message))
        }
    }
    if (-not $near.Count) { Say '   nothing else logged in this window' 'Green' }
}

# ------------------------------------------------------------------ TermService crashes

$crashSince = (Get-Date).AddDays(-$CrashDays)
$tsCrash = @(Get-WinEvent -ErrorAction SilentlyContinue -FilterHashtable @{
        LogName = 'Application'; Id = 1000; StartTime = $crashSince } |
    Where-Object { $_.Message -match 'TermService' } | Sort-Object TimeCreated)
$tsScm = @(Get-WinEvent -ErrorAction SilentlyContinue -FilterHashtable @{
        LogName = 'System'; ProviderName = 'Service Control Manager'; Id = 7031, 7034; StartTime = $crashSince } |
    Where-Object { $_.Message -match 'Remote Desktop Services' } | Sort-Object TimeCreated)

Say ''
Say "== TermService crashes, last $CrashDays days: $($tsCrash.Count) crash record(s), $($tsScm.Count) service-terminated event(s) ==" 'Cyan'
if (-not ($tsCrash.Count -or $tsScm.Count)) { Say '  none' 'Green' }
foreach ($e in $tsCrash) {
    $c = Get-CrashFields $e.Message
    Say ('  {0:yyyy-MM-dd HH:mm:ss}  module {1}  exception {2}  offset {3}' -f $e.TimeCreated, $c.Module, $c.Exception, $c.Offset) 'Yellow'
}
if (-not $tsCrash.Count) {
    foreach ($e in $tsScm) { Say ('  {0:yyyy-MM-dd HH:mm:ss}  SCM {1}: {2}' -f $e.TimeCreated, $e.Id, (Get-FirstLine $e.Message)) 'Yellow' }
}
if ($tdll -and $tsCrash.Count) {
    $after = @($tsCrash | Where-Object TimeCreated -gt $tdll.LastWriteTime).Count
    Say ('  {0} crash(es) before termsrv.dll was last written ({1:yyyy-MM-dd}), {2} after' -f ($tsCrash.Count - $after), $tdll.LastWriteTime, $after)
}

# ------------------------------------------------------------------ who can reset a loopback socket

Say ''
Say '== WFP providers and how many filters each owns ==' 'Cyan'
$wfp = Join-Path $OutRoot ('wfpstate-{0}.xml' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$null = & netsh wfp show state file="$wfp" 2>&1
if (Test-Path -LiteralPath $wfp) {
    try {
        $w = Get-WfpProviderRows -Path $wfp
        if ($w.Rows.Count) {
            $w.Rows | Sort-Object @{ Expression = 'Filters'; Descending = $true }, Name |
                ForEach-Object { Say ('  {0,5}  {1}' -f $_.Filters, $_.Name) }
        } else {
            Say "  no provider entries; the file's root element is <$($w.Root)> - kept at $wfp" 'Yellow'
        }
    } catch {
        Say "  could not parse ${wfp}: $($_.Exception.Message)" 'Red'
    }
} else {
    Say '  netsh wfp show state wrote no file' 'Yellow'
}

Say ''
Say '== running kernel drivers not published by Microsoft ==' 'Cyan'
Get-CimInstance Win32_SystemDriver -Filter "State='Running'" | Sort-Object Name | ForEach-Object {
    $p = "$($_.PathName)" -replace '^\\\?\?\\' -replace '^\\SystemRoot', $env:SystemRoot -replace '^system32', "$env:SystemRoot\system32"
    $c = $null
    if ($p) { $c = (Get-Item -LiteralPath $p -ErrorAction SilentlyContinue).VersionInfo.CompanyName }
    if ("$c" -notmatch 'Microsoft') { Say ('  {0,-22} {1,-30} {2}' -f $_.Name, $c, $p) }
}

Say ''
Say "Saved: $outFile" 'Cyan'
