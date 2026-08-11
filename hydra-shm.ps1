#requires -Version 5.1
<#
    hydra-shm.ps1 — read seat state from shared memory instead of from logs.

    capture_B.log is startup-only. router.log is startup-only. hydractl status
    reports process liveness, not work done. The authoritative signals are in
    the two named sections that hydra_ipc.h defines, and nothing was reading
    them:

      Global\HydraSeat_<seat>_meta   HydraSeatMeta   — ready, frame, STALLED
      Global\HydraSeat_<seat>_pix    HydraSeatPixels — seq, dims, cursor

    `stalled` is the retry count for "attached to the desktop but EnumOutputs
    returns no duplicatable display" — the exact failure mode that looks
    identical to healthy-and-idle from the outside. `seq` increments per
    published frame; flat seq is a stopped pipeline, not an inference.

    Usage:
        .\hydra-shm.ps1                 # one snapshot
        .\hydra-shm.ps1 -Watch          # live, 1s
        .\hydra-shm.ps1 -Seat B -Watch -IntervalSec 2

    Must run ELEVATED — the sections are in the Global\ namespace.
#>
[CmdletBinding()]
param(
    [string] $Seat        = 'B',
    [switch] $Watch,
    [int]    $IntervalSec = 1
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HydraShm {
    const uint FILE_MAP_READ = 0x0004;
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr OpenFileMappingW(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr MapViewOfFile(IntPtr h, uint access, uint hi, uint lo, UIntPtr bytes);
    [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr addr);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    // Returns the first `count` bytes of the section, or null.
    // lastError is 2 = not found, 5 = access denied (not elevated).
    public static byte[] Read(string name, int count, out int lastError) {
        lastError = 0;
        IntPtr h = OpenFileMappingW(FILE_MAP_READ, false, name);
        if (h == IntPtr.Zero) { lastError = Marshal.GetLastWin32Error(); return null; }
        IntPtr v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, (UIntPtr)(uint)count);
        if (v == IntPtr.Zero) { lastError = Marshal.GetLastWin32Error(); CloseHandle(h); return null; }
        byte[] b = new byte[count];
        Marshal.Copy(v, b, 0, count);
        UnmapViewOfFile(v);
        CloseHandle(h);
        return b;
    }
}
'@ -ErrorAction Stop

# Offsets derived from hydra_ipc.h with natural alignment.
#
# HydraSeatMeta (48 bytes)
#   0  version u32      4  ready u32       8  width u32      12 height u32
#   16 dxgiFormat u32   20 luidLow u32     24 luidHigh i32   (28 pad)
#   32 frame u64        40 generation u32  44 stalled u32
#
# HydraSeatPixels (40 bytes of header, then w*h*4 BGRA)
#   0  seq u64          8  width u32       12 height u32     16 pitch u32
#   20 _pad u32         24 curX i32        28 curY i32       32 curSeq u32

function Get-HydraMeta([string]$seat) {
    $err = 0
    $b = [HydraShm]::Read("Global\HydraSeat_${seat}_meta", 48, [ref]$err)
    if (-not $b) { return [pscustomobject]@{ Error = $err } }
    [pscustomobject]@{
        Error      = 0
        Version    = [BitConverter]::ToUInt32($b, 0)
        Ready      = [BitConverter]::ToUInt32($b, 4)
        Width      = [BitConverter]::ToUInt32($b, 8)
        Height     = [BitConverter]::ToUInt32($b, 12)
        Format     = [BitConverter]::ToUInt32($b, 16)
        LuidLow    = [BitConverter]::ToUInt32($b, 20)
        LuidHigh   = [BitConverter]::ToInt32($b, 24)
        Frame      = [BitConverter]::ToUInt64($b, 32)
        Generation = [BitConverter]::ToUInt32($b, 40)
        Stalled    = [BitConverter]::ToUInt32($b, 44)
    }
}

function Get-HydraPix([string]$seat) {
    $err = 0
    $b = [HydraShm]::Read("Global\HydraSeat_${seat}_pix", 40, [ref]$err)
    if (-not $b) { return [pscustomobject]@{ Error = $err } }
    [pscustomobject]@{
        Error  = 0
        Seq    = [BitConverter]::ToUInt64($b, 0)
        Width  = [BitConverter]::ToUInt32($b, 8)
        Height = [BitConverter]::ToUInt32($b, 12)
        Pitch  = [BitConverter]::ToUInt32($b, 16)
        CurX   = [BitConverter]::ToInt32($b, 24)
        CurY   = [BitConverter]::ToInt32($b, 28)
        CurSeq = [BitConverter]::ToUInt32($b, 32)
    }
}

function Get-HydraAudio([string]$seat) {
    # HydraAudioRing: 0 writePos u64, 8 rate u32, 12 channels u32,
    #                 16 ringFrames u32, 20 running u32
    $err = 0
    $b = [HydraShm]::Read("Global\HydraSeat_${seat}_aud", 24, [ref]$err)
    if (-not $b) { return [pscustomobject]@{ Error = $err } }
    [pscustomobject]@{
        Error      = 0
        WritePos   = [BitConverter]::ToUInt64($b, 0)
        Rate       = [BitConverter]::ToUInt32($b, 8)
        Channels   = [BitConverter]::ToUInt32($b, 12)
        RingFrames = [BitConverter]::ToUInt32($b, 16)
        Running    = [BitConverter]::ToUInt32($b, 20)
    }
}

function Explain([int]$err) {
    switch ($err) {
        0 { '' }
        2 { 'ERROR_FILE_NOT_FOUND -- section does not exist; producer never started' }
        5 { 'ERROR_ACCESS_DENIED -- run this shell ELEVATED' }
        default { "win32 error $err" }
    }
}

function Show-Once([string]$seat, $prevPix, $prevMeta, $prevAud, [double]$dt) {
    $m = Get-HydraMeta  $seat
    $p = Get-HydraPix   $seat
    $a = Get-HydraAudio $seat

    Write-Host ("--- seat {0}  {1:HH:mm:ss} " -f $seat, (Get-Date)) -ForegroundColor Cyan

    if ($p.Error -ne 0) {
        Write-Host ("  pix   UNAVAILABLE  {0}" -f (Explain $p.Error)) -ForegroundColor Red
    } else {
        $rate = ''
        if ($prevPix -and $dt -gt 0) {
            $d = [double]($p.Seq - $prevPix.Seq)
            $fps = [math]::Round($d / $dt / 2, 1)   # seq bumps twice per frame (odd->even)
            $rate = "  ~$fps fps"
        }
        $col = 'Green'
        if ($prevPix -and $p.Seq -eq $prevPix.Seq) { $col = 'Yellow' }
        Write-Host ("  pix   seq={0}{1}  {2}x{3} pitch={4}" -f $p.Seq, $rate, $p.Width, $p.Height, $p.Pitch) -ForegroundColor $col
        Write-Host ("  cur   ({0},{1}) curSeq={2}{3}" -f $p.CurX, $p.CurY, $p.CurSeq,
            $(if ($p.CurSeq -eq 0) { '   <-- never published (PROBLEM 3)' } else { '' }))
    }

    if ($m.Error -ne 0) {
        Write-Host ("  meta  UNAVAILABLE  {0}" -f (Explain $m.Error)) -ForegroundColor DarkGray
    } else {
        Write-Host ("  meta  ready={0} frame={1} gen={2} {3}x{4} fmt={5}" -f `
            $m.Ready, $m.Frame, $m.Generation, $m.Width, $m.Height, $m.Format)
        if ($m.Stalled -ne 0) {
            Write-Host ("  STALLED={0}  producer attached to the desktop but EnumOutputs" -f $m.Stalled) -ForegroundColor Red
            Write-Host  "            returns no duplicatable display. This is the failure that" -ForegroundColor Red
            Write-Host  "            looks identical to healthy-and-idle from outside." -ForegroundColor Red
        }
    }

    if ($a.Error -eq 0) {
        $arate = ''
        if ($prevAud -and $dt -gt 0) {
            $d = [double]($a.WritePos - $prevAud.WritePos)
            $arate = "  {0} frames/s" -f [math]::Round($d / $dt)
        }
        Write-Host ("  aud   writePos={0}{1}  {2}Hz x{3} running={4}" -f `
            $a.WritePos, $arate, $a.Rate, $a.Channels, $a.Running)
    }

    return @{ Pix = $p; Meta = $m; Aud = $a }
}

if (-not $Watch) {
    [void](Show-Once $Seat $null $null $null 0)
    return
}

Write-Host "watching seat $Seat every ${IntervalSec}s. Ctrl-C to stop.`n" -ForegroundColor Cyan
$prev = $null
$last = Get-Date
while ($true) {
    $now = Get-Date
    $dt  = ($now - $last).TotalSeconds
    $cur = Show-Once $Seat $prev.Pix $prev.Meta $prev.Aud $dt
    $prev = $cur
    $last = $now
    Start-Sleep -Seconds $IntervalSec
}
