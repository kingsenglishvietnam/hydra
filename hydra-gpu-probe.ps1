# hydra-gpu-probe.ps1 -- what is seat B's video costing, and is the H.264 recipe in place?
#
# Written 2026-10-08 to test the README's claim that seat B is composed in
# software (WARP). It isn't: its dwm draws on the Iris Plus. The cost was
# RemoteFX, encoded and decoded on the CPU. VIDEO-H264.md has the runs this
# found; this is the instrument, kept.
#
# REPORT (default, read-only) over one window, normally 60 s of video:
#   - the recipe's registry values, now against target
#   - CPU-seconds per process, seat B's marked
#   - which adapter seat B's dwm and apps draw and decode on (GPU Engine
#     counters, LUIDs named through DXGI; Basic Render has the software flag)
#   - RdpCoreTS 162/170: H.264 negotiated? encoded in hardware, by whom?
#   - RDPClient, last 12 h: UDP attempts (1102), socket-closed drops (2308)
#   - whether Hyper-V GPU-P can partition a GPU here (the VM route)
#
# THE RECIPE. -Apply sets it, after a snapshot under safety\; -Revert puts
# back exactly what the snapshot recorded. Reboot after either.
#   Terminal Services policy        AVC444ModePreferred = 1
#                                   AVCHardwareEncodePreferred = 1
#                                   bEnumerateHWBeforeSW ABSENT. Set, it moved
#                                   seat B onto the 1660 Ti, whose H.264
#                                   encoder crashed RdpIdd.
#   ...\Terminal Services\Client    fClientDisableUDP = 1. UDP on the loopback
#                                   dropped the session every few minutes.
#   WinStations                     DWMFRAMEINTERVAL = 15. 60 fps, not RDP's 30.
# -Avc is accepted and ignored: version 1 split the recipe, this one doesn't.
#
# USAGE (elevated, Hydra Shell; hydra7-avc up, the same clip full-screen on seat B)
#   .\hydra-gpu-probe.ps1
#   .\hydra-gpu-probe.ps1 -Apply          (then reboot)
#   .\hydra-gpu-probe.ps1 -Revert         (then reboot)
#   .\hydra-gpu-probe.ps1 -Seconds 30 -NoPrompt
#
# Every run is transcribed to tests\gpu-probe-<stamp>.txt.

param(
    [switch]$Apply,
    [switch]$Avc,
    [switch]$Revert,
    [int]$Seconds = 60,
    [string]$User = 'teacher',
    [switch]$NoPrompt,
    [string]$Root = 'C:\Programs\hydra'
)

$ErrorActionPreference = 'Continue'
$PSNativeCommandUseErrorActionPreference = $false
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Say 'Run elevated (Hydra Shell).' Red; return }
if ($Apply -and $Revert) { Say '-Apply and -Revert together? Pick one.' Red; return }
if ($Avc) { Say '-Avc is ignored now: -Apply sets the whole recipe.' DarkGray }

$PolicyKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services'
$WinStaKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations'
$ClientKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services\Client'
$SafetyDir = Join-Path $Root 'safety'

# Want: a DWORD to set, 'absent' to remove, or $null for report-only.
$Settings = @(
    [pscustomobject]@{ Key = $PolicyKey; Name = 'AVC444ModePreferred'; Want = 1
        What = 'H.264 4:4:4' }
    [pscustomobject]@{ Key = $PolicyKey; Name = 'AVCHardwareEncodePreferred'; Want = 1
        What = 'H.264 encoded on the GPU' }
    [pscustomobject]@{ Key = $PolicyKey; Name = 'bEnumerateHWBeforeSW'; Want = 'absent'
        What = 'set, it moves seat B onto the 1660 Ti (NVENC crashes RdpIdd)' }
    [pscustomobject]@{ Key = $ClientKey; Name = 'fClientDisableUDP'; Want = 1
        What = 'TCP only; UDP on the loopback drops the session' }
    [pscustomobject]@{ Key = $WinStaKey; Name = 'DWMFRAMEINTERVAL'; Want = 15
        What = '60 fps cap (absent = 30)' }
    [pscustomobject]@{ Key = $PolicyKey; Name = 'fEnableWddmDriver'; Want = $null
        What = 'report only (absent = WDDM, the path rdpidd runs on)' }
)

function Get-RegValue($key, $name) {
    $p = Get-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
    if ($null -eq $p) { return $null }
    $p.$name
}

# NOT New-Item -Force: on the registry provider that recreates an existing key
# EMPTY, and the policy key holds every other RDS policy. Parents one at a time.
function New-KeyIfMissing([string]$path) {
    if (Test-Path $path) { return }
    $i = $path.LastIndexOf('\')
    if ($i -gt 0) { New-KeyIfMissing $path.Substring(0, $i) }
    New-Item -Path $path | Out-Null
}

# The newest snapshot that has not been reverted holds the true originals.
# -Apply reuses it rather than snapshotting already-applied values.
function Get-OpenSnapshot {
    Get-ChildItem -Path $SafetyDir -Directory -Filter 'gpu-probe-*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'registry-before.json' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logDir = Join-Path $Root 'tests'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
Start-Transcript -Path (Join-Path $logDir "gpu-probe-$stamp.txt") | Out-Null

try {

# ------------------------------------------------------------- -Apply ------
if ($Apply) {
    $snap = Get-OpenSnapshot
    if ($snap) {
        # Keep the true originals. Add rows for anything an older snapshot
        # lacks (version 1 did not know fClientDisableUDP), as they stand now.
        $rows = @()
        foreach ($r in (Get-Content -Raw -Path $snap | ConvertFrom-Json)) { $rows += $r }
        $added = 0
        foreach ($s in $Settings) {
            if (-not ($rows | Where-Object { $_.Key -eq $s.Key -and $_.Name -eq $s.Name })) {
                $v = Get-RegValue $s.Key $s.Name
                $rows += [pscustomobject]@{ Key = $s.Key; Name = $s.Name; Existed = ($null -ne $v); Value = $v }
                $added++
            }
        }
        if ($added) { ConvertTo-Json -InputObject $rows | Set-Content -Path $snap -Encoding UTF8 }
        Say "keeping the existing snapshot (the true originals): $snap" DarkGray
    } else {
        $dir = Join-Path $SafetyDir "gpu-probe-$stamp"
        New-Item -ItemType Directory -Path $dir | Out-Null
        $rows = foreach ($s in $Settings) {
            $v = Get-RegValue $s.Key $s.Name
            [pscustomobject]@{ Key = $s.Key; Name = $s.Name; Existed = ($null -ne $v); Value = $v }
        }
        $snap = Join-Path $dir 'registry-before.json'
        $rows | ConvertTo-Json | Set-Content -Path $snap -Encoding UTF8
        Say "snapshot: $snap" DarkGray
    }
    foreach ($s in $Settings) {
        if ($null -eq $s.Want) { continue }
        if ("$($s.Want)" -eq 'absent') {
            if ($null -ne (Get-RegValue $s.Key $s.Name)) {
                Remove-ItemProperty -Path $s.Key -Name $s.Name
                Say ('  removed {0}' -f $s.Name) Green
            }
            continue
        }
        New-KeyIfMissing $s.Key
        Set-ItemProperty -Path $s.Key -Name $s.Name -Value $s.Want -Type DWord
        Say ('  set {0} = {1}' -f $s.Name, $s.Want) Green
    }
    Say 'Reboot. Then hydra7-avc, the same video on seat B, and this script again.' Cyan
    return
}

# ------------------------------------------------------------ -Revert ------
if ($Revert) {
    $snap = Get-OpenSnapshot
    if (-not $snap) { Say 'No open gpu-probe snapshot under safety\ -- nothing to revert.' Yellow; return }
    foreach ($r in (Get-Content -Raw -Path $snap | ConvertFrom-Json)) {
        if ($r.Existed) {
            Set-ItemProperty -Path $r.Key -Name $r.Name -Value ([int]$r.Value) -Type DWord
            Say ('  restored {0} = {1}' -f $r.Name, $r.Value) Green
        } elseif ($null -ne (Get-RegValue $r.Key $r.Name)) {
            Remove-ItemProperty -Path $r.Key -Name $r.Name
            Say ('  removed {0} (it was absent before)' -f $r.Name) Green
        }
    }
    Rename-Item -Path $snap -NewName 'registry-before.reverted.json'
    Say 'Reverted. Reboot for it to take effect.' Cyan
    return
}

# ------------------------------------------------------------ report -------
Say ''
Say '== registry ==' Cyan
foreach ($s in $Settings) {
    $v = Get-RegValue $s.Key $s.Name
    $cur = if ($null -eq $v) { '(absent)' } else { "$v" }
    $tgt = if ($null -eq $s.Want) { '-' } elseif ("$($s.Want)" -eq 'absent') { '(absent)' } else { "$($s.Want)" }
    $col = if ($null -eq $s.Want) { 'Gray' } elseif ($cur -eq $tgt) { 'Green' } else { 'Yellow' }
    Say ('  {0,-27} now {1,-9} target {2,-9} {3}' -f $s.Name, $cur, $tgt, $s.What) $col
}

# DXGI from the console names adapters by LUID; the GPU Engine counters
# report work by LUID. Basic Render is the one with the software flag.
if (-not ('HydraProbe.Dxgi' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace HydraProbe {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct AdapterDesc1 {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public UIntPtr DedicatedVideoMemory;
        public UIntPtr DedicatedSystemMemory;
        public UIntPtr SharedSystemMemory;
        public uint LuidLow;
        public int LuidHigh;
        public uint Flags;
    }

    // Vtable order is all that matters for methods never called.
    [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIAdapter1 {
        void SetPrivateData(); void SetPrivateDataInterface();
        void GetPrivateData(); void GetParent();
        void EnumOutputs(); void GetDesc(); void CheckInterfaceSupport();
        [PreserveSig] int GetDesc1(out AdapterDesc1 desc);
    }

    [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIFactory1 {
        void SetPrivateData(); void SetPrivateDataInterface();
        void GetPrivateData(); void GetParent();
        void EnumAdapters(); void MakeWindowAssociation();
        void GetWindowAssociation(); void CreateSwapChain();
        void CreateSoftwareAdapter();
        [PreserveSig] int EnumAdapters1(uint index, out IDXGIAdapter1 adapter);
    }

    public class Adapter {
        public string Luid;
        public string Name;
        public bool Software;
    }

    public static class Dxgi {
        [DllImport("dxgi.dll")]
        static extern int CreateDXGIFactory1(ref Guid riid,
            [MarshalAs(UnmanagedType.Interface)] out IDXGIFactory1 factory);

        public static List<Adapter> List() {
            List<Adapter> result = new List<Adapter>();
            Guid iid = typeof(IDXGIFactory1).GUID;
            IDXGIFactory1 f;
            if (CreateDXGIFactory1(ref iid, out f) != 0) return result;
            for (uint i = 0; ; i++) {
                IDXGIAdapter1 a;
                if (f.EnumAdapters1(i, out a) != 0) break;
                AdapterDesc1 d;
                if (a.GetDesc1(out d) == 0) {
                    Adapter x = new Adapter();
                    x.Luid = string.Format("0x{0:x8}_0x{1:x8}", d.LuidHigh, d.LuidLow);
                    x.Name = d.Description;
                    x.Software = (d.Flags & 2) != 0;
                    result.Add(x);
                }
                Marshal.ReleaseComObject(a);
            }
            Marshal.ReleaseComObject(f);
            return result;
        }
    }
}
'@
}

Say ''
Say '== adapters, console view ==' Cyan
$adapters = @{}
try {
    foreach ($a in [HydraProbe.Dxgi]::List()) {
        $adapters[$a.Luid] = $a
        $flag = if ($a.Software) { '  [software]' } else { '' }
        Say ('  {0}  {1}{2}' -f $a.Luid, $a.Name, $flag)
    }
} catch { Say "  DXGI enumeration failed: $($_.Exception.Message)" Yellow }

function Get-SeatSessionId([string]$Name) {
    Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" |
        ForEach-Object {
            $o = Invoke-CimMethod -InputObject $_ -MethodName GetOwner
            if ($o.User -eq $Name) { [int]$_.SessionId }
        } |
        Select-Object -First 1
}

function Get-CpuSnapshot {
    $t = @{}
    foreach ($p in Get-Process) {
        try {
            $c = $p.TotalProcessorTime
            if ($null -ne $c) {
                $t[$p.Id] = [pscustomobject]@{ Name = $p.ProcessName; Session = $p.SessionId; Cpu = $c.TotalSeconds }
            }
        } catch { }
    }
    $t
}

$sid = Get-SeatSessionId $User
Say ''
if ($null -eq $sid) {
    Say "No logged-in session for '$User' -- bring seat B up with hydra7 first. Skipping the sample." Yellow
} else {
    Say "seat B = session $sid ($User)" Cyan
    if (-not $NoPrompt) {
        Read-Host 'Play the test video full-screen on seat B, then press Enter here' | Out-Null
    }
    $n = [Math]::Max(1, [int][Math]::Round($Seconds / 2))
    Say "sampling for ~$($n * 2) s -- leave both seats alone ..." DarkGray
    $before = Get-CpuSnapshot
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $sets = @(Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' `
        -SampleInterval 2 -MaxSamples $n -ErrorAction SilentlyContinue)
    # If the counters failed fast, still hold the CPU window open its full length.
    $left = ($n * 2) - $sw.Elapsed.TotalSeconds
    if ($left -gt 1) { Start-Sleep -Seconds ([int]$left) }
    $sw.Stop()
    $after = Get-CpuSnapshot
    $secs = $sw.Elapsed.TotalSeconds

    # --- CPU: delta over the window, not cumulative .CPU (see a7d1f9d) ---
    $rows = foreach ($k in $after.Keys) {
        if ($k -eq $PID -or $k -eq 0) { continue }
        $a = $after[$k]
        $b = $before[$k]
        $d = if ($b -and $b.Name -eq $a.Name) { $a.Cpu - $b.Cpu } else { $a.Cpu }
        [pscustomobject]@{ Name = $a.Name; Id = $k; Session = $a.Session; CpuSec = [Math]::Round($d, 1) }
    }
    $total = ($rows | Measure-Object CpuSec -Sum).Sum
    Say ''
    Say ('== CPU over {0:N0} s: {1:N0} CPU-sec, {2:N1} logical processors busy on average (probe excluded) ==' -f `
        $secs, $total, ($total / $secs)) Cyan
    foreach ($r in ($rows | Sort-Object CpuSec -Descending | Select-Object -First 10)) {
        $tag = if ($r.Session -eq $sid) { '   <- seat B' } else { '' }
        Say ('  {0,7:N1}  {1,-22} pid {2,-6} session {3}{4}' -f $r.CpuSec, $r.Name, $r.Id, $r.Session, $tag)
    }

    # --- GPU: which engines on which adapter, for seat B's processes ---
    $seatPids = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($t in @($before, $after)) {
        foreach ($k in $t.Keys) { if ($t[$k].Session -eq $sid) { [void]$seatPids.Add([int]$k) } }
    }
    $acc = @{}
    foreach ($set in $sets) {
        foreach ($s in $set.CounterSamples) {
            if ($s.Status -ne 0) { continue }
            if ($s.InstanceName -match '^pid_(\d+)_luid_(0x[0-9a-f]{8}_0x[0-9a-f]{8})_phys_\d+_eng_\d+_engtype_(.+)$') {
                $procId = [int]$Matches[1]
                if (-not $seatPids.Contains($procId)) { continue }
                $key = '{0}|{1}|{2}' -f $procId, $Matches[2].ToLower(), $Matches[3].ToLower()
                $acc[$key] = [double]$acc[$key] + $s.CookedValue
            }
        }
    }
    $okSets = [Math]::Max(1, $sets.Count)
    $gpuRows = foreach ($key in $acc.Keys) {
        $p = $key.Split('|')
        $procId = [int]$p[0]
        $nm = '?'
        if ($after[$procId]) { $nm = $after[$procId].Name } elseif ($before[$procId]) { $nm = $before[$procId].Name }
        $ad = $adapters[$p[1]]
        [pscustomobject]@{
            Proc    = $nm
            Luid    = $p[1]
            Adapter = if ($ad) { $ad.Name } else { "(not visible from the console: $($p[1]))" }
            Soft    = if ($ad) { $ad.Software } else { $null }
            Engine  = $p[2]
            AvgPct  = [Math]::Round($acc[$key] / $okSets, 1)
        }
    }

    Say ''
    Say '== GPU engines used by seat B processes (average % over the window) ==' Cyan
    if ($sets.Count -eq 0) {
        Say '  GPU Engine counters unavailable -- use Task Manager on seat B, GPU engine column.' Yellow
    }
    $show = @($gpuRows | Where-Object { $_.AvgPct -ge 0.1 -or $_.Proc -eq 'dwm' } | Sort-Object AvgPct -Descending)
    if ($show.Count -eq 0) { Say '  none -- nothing in seat B touched a GPU engine during the sample.' Yellow }
    foreach ($g in $show) {
        Say ('  {0,6:N1}%  {1,-16} {2,-18} {3}' -f $g.AvgPct, $g.Proc, $g.Engine, $g.Adapter)
    }

    # --- verdict ---
    $dwmCpu = ($rows | Where-Object { $_.Name -eq 'dwm' -and $_.Session -eq $sid } | Measure-Object CpuSec -Sum).Sum
    $dwmGpu = @($gpuRows | Where-Object { $_.Proc -eq 'dwm' } | Group-Object Luid | ForEach-Object {
        [pscustomobject]@{
            Adapter = $_.Group[0].Adapter
            Soft    = $_.Group[0].Soft
            Pct     = ($_.Group | Measure-Object AvgPct -Sum).Sum
        }
    } | Sort-Object Pct -Descending)
    Say ''
    Say '== verdict ==' Cyan
    if ($dwmGpu.Count -eq 0) {
        Say "  seat B's dwm.exe used no GPU engine: SOFTWARE composition (or nothing moved)." Red
    } elseif ($dwmGpu[0].Soft -eq $true) {
        Say "  seat B composes on $($dwmGpu[0].Adapter): SOFTWARE (WARP)." Red
    } elseif ($dwmGpu[0].Soft -eq $false) {
        Say "  seat B composes on $($dwmGpu[0].Adapter): HARDWARE." Green
    } else {
        Say "  seat B composes on an adapter instance private to its session $($dwmGpu[0].Adapter)." Yellow
        Say '  Not one the console can name -- let the dwm CPU figure below decide it.' Yellow
    }
    Say ('  seat B dwm.exe: {0:N1} CPU-sec over {1:N0} s. WARP pays for every composed frame here; a GPU makes it nearly nothing.' -f `
        $dwmCpu, $secs)
    $dec = @($gpuRows | Where-Object { $_.Engine -match 'decode' -and $_.AvgPct -ge 0.5 })
    if ($dec.Count -gt 0) {
        $txt = ($dec | ForEach-Object { '{0} {1}% on {2}' -f $_.Proc, $_.AvgPct, $_.Adapter }) -join '; '
        Say "  video decode on a GPU: $txt" Green
    } else {
        Say '  no GPU video decode in seat B -- the browser decoded on the CPU, or nothing played.'
    }
}

# ---------------------------------------------------- RDP codec events ---
Say ''
Say '== RdpCoreTS 162/170 (AVC 4:4:4 mode, hardware encode), last 12 h ==' Cyan
$ev = @(Get-WinEvent -FilterHashtable @{
        LogName   = 'Microsoft-Windows-RemoteDesktopServices-RdpCoreTS/Operational'
        Id        = 162, 170
        StartTime = (Get-Date).AddHours(-12) } `
    -MaxEvents 6 -ErrorAction SilentlyContinue)
if ($ev.Count -eq 0) { Say '  none -- expected unless H.264 is on and mstsc has connected.' }
foreach ($e in $ev) {
    Say ('  {0:HH:mm:ss}  {1}  {2}' -f $e.TimeCreated, $e.Id, (($e.Message -split "`r?`n")[0]))
}

# --------------------------------------------- client drops and UDP -----
# 1102 = mstsc opened a UDP transport (fClientDisableUDP should stop that).
# 1026 with Reason=2308 = the socket closed under it: the drops UDP caused.
Say ''
Say '== RDPClient, last 12 h: UDP attempts (1102), socket-closed drops (2308) ==' Cyan
$cl = @(Get-WinEvent -FilterHashtable @{
        LogName   = 'Microsoft-Windows-TerminalServices-RDPClient/Operational'
        Id        = 1026, 1102
        StartTime = (Get-Date).AddHours(-12) } `
    -ErrorAction SilentlyContinue)
$udp   = @($cl | Where-Object { $_.Id -eq 1102 })
$drops = @($cl | Where-Object { $_.Id -eq 1026 -and $_.Message -match 'Reason= ?2308' })
$clCol = if ($udp.Count -or $drops.Count) { 'Yellow' } else { 'Green' }
Say ('  UDP attempts: {0}   socket-closed drops: {1}' -f $udp.Count, $drops.Count) $clCol
foreach ($e in ($drops | Sort-Object TimeCreated | Select-Object -Last 5)) {
    Say ('  {0:HH:mm:ss}  drop' -f $e.TimeCreated)
}

# ------------------------------------------------ the VM route (GPU-P) ---
Say ''
Say '== the VM route, for when the verdict says SOFTWARE ==' Cyan
$os = Get-CimInstance Win32_OperatingSystem
Say ('  {0}, build {1}' -f $os.Caption, $os.BuildNumber)
if ($os.Caption -match 'Home') { Say '  Home edition: no Hyper-V, so no GPU-P without an upgrade.' Yellow }
if (-not (Get-Service vmms -ErrorAction SilentlyContinue)) {
    Say '  Hyper-V not installed, so partitionability is unknown. Enabling it is boot-affecting:' Gray
    Say '  safety-gate.ps1 first, on a reboot of its own, then run this again.' Gray
} elseif (-not (Get-Command Get-VMHostPartitionableGpu -ErrorAction SilentlyContinue)) {
    Say '  Hyper-V present, Get-VMHostPartitionableGpu missing (Windows 10 calls it Get-VMPartitionableGpu).' Yellow
} else {
    $gp = @(Get-VMHostPartitionableGpu -ErrorAction SilentlyContinue)
    if ($gp.Count -eq 0) { Say '  no partitionable GPU: the VM route is closed on this machine.' Red }
    foreach ($g in $gp) {
        $vendor = switch -Regex ($g.Name) {
            'VEN_8086' { 'Intel'; break }
            'VEN_10DE' { 'NVIDIA'; break }
            'VEN_1002' { 'AMD'; break }
            default    { '?' }
        }
        Say ('  partitionable: {0}  {1}' -f $vendor, $g.Name) Green
    }
}

} finally {
    Stop-Transcript | Out-Null
}
