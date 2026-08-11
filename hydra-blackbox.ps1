#requires -Version 5.1
<#
    hydra-blackbox.ps1 — rolling evidence recorder for Hydra PROBLEM 1

    Runs continuously in a spare console. Samples the pipeline every few
    seconds into a rolling in-memory buffer. When the pipeline stalls it
    writes the buffer — the minutes BEFORE the stall, which is the part
    that matters — to STALL-<timestamp>.txt and keeps going.

    You never have to remember to run ON-LOCKUP.md again. Restart the seat
    the instant it dies; the evidence is already on disk.

    Usage:
        cd C:\Programs\hydra
        .\hydra-blackbox.ps1

        .\hydra-blackbox.ps1 -IntervalSec 3 -StallSamples 10 -Verbose
#>
[CmdletBinding()]
param(
    [string] $Seat          = 'B',
    [int]    $IntervalSec   = 5,
    # consecutive flat samples before declaring a stall (6 x 5s = 30s)
    [int]    $StallSamples  = 6,
    # rolling buffer depth (360 x 5s = 30 minutes)
    [int]    $BufferSamples = 360,
    [string] $HydraRoot     = 'C:\Programs\hydra',
    [string] $LogDir        = 'C:\ProgramData\Hydra\logs',
    [string[]] $Watch       = @('sdl-freerdp','mirror','hydrardp','seatB_agent','seat_router','audio_bridge','cursorfence')
)

$ErrorActionPreference = 'Continue'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class HydraWin {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int n);
    // Is any part of hWnd actually visible, or is it fully covered?
    public static string Title(IntPtr h) {
        StringBuilder sb = new StringBuilder(512);
        GetWindowTextW(h, sb, 512);
        return sb.ToString();
    }
    public static string Describe(IntPtr h) {
        if (h == IntPtr.Zero) return "hwnd=0";
        RECT r;
        GetWindowRect(h, out r);
        return string.Format("hwnd=0x{0:X} iconic={1} visible={2} rect=({3},{4})-({5},{6}) title=\"{7}\"",
            h.ToInt64(), IsIconic(h), IsWindowVisible(h), r.Left, r.Top, r.Right, r.Bottom, Title(h));
    }
}
'@ -ErrorAction Stop

$capLog   = Join-Path $LogDir "capture_$Seat.log"
$hydractl = Join-Path $HydraRoot 'dist\hydractl.exe'
$stream   = Join-Path $LogDir ("blackbox-{0}.log" -f (Get-Date -Format 'yyyyMMdd'))

function Get-Sample {
    $s = [ordered]@{
        Time      = Get-Date
        Procs     = @{}
        CapLen    = -1
        CapWrite  = $null
        CapTail   = @()
        Foreground= ''
        Windows   = @()
        Sessions  = @()
        Status    = @()
    }

    foreach ($n in $Watch) {
        $p = Get-Process -Name $n -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($p) {
            $s.Procs[$n] = [ordered]@{
                Id        = $p.Id
                CPU       = [math]::Round($p.TotalProcessorTime.TotalSeconds, 3)
                WS_MB     = [math]::Round($p.WorkingSet64 / 1MB, 1)
                Threads   = $p.Threads.Count
                Handles   = $p.HandleCount
                Responding= $p.Responding
            }
            if ($p.MainWindowHandle -ne 0) {
                $s.Windows += ("{0}: {1}" -f $n, [HydraWin]::Describe($p.MainWindowHandle))
            }
        } else {
            $s.Procs[$n] = $null
        }
    }

    if (Test-Path $capLog) {
        $fi = Get-Item $capLog
        $s.CapLen   = $fi.Length
        $s.CapWrite = $fi.LastWriteTime
        $s.CapTail  = @(Get-Content $capLog -Tail 4 -ErrorAction SilentlyContinue)
    }

    $s.Foreground = [HydraWin]::Describe([HydraWin]::GetForegroundWindow())
    $s.Sessions   = @(& query session 2>&1)
    if (Test-Path $hydractl) { $s.Status = @(& $hydractl status 2>&1) }
    return $s
}

function Format-Sample($s) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("=== {0:yyyy-MM-dd HH:mm:ss.fff} ===" -f $s.Time)
    foreach ($k in $s.Procs.Keys) {
        $v = $s.Procs[$k]
        if ($null -eq $v) { [void]$sb.AppendLine("  proc {0,-13} ABSENT" -f $k) }
        else {
            [void]$sb.AppendLine(("  proc {0,-13} pid={1,-6} cpu={2,-10} ws={3,-7}MB thr={4,-4} hnd={5,-6} resp={6}" -f `
                $k, $v.Id, $v.CPU, $v.WS_MB, $v.Threads, $v.Handles, $v.Responding))
        }
    }
    [void]$sb.AppendLine(("  caplog len={0} lastwrite={1}" -f $s.CapLen, $s.CapWrite))
    foreach ($l in $s.CapTail)  { [void]$sb.AppendLine("    | $l") }
    foreach ($w in $s.Windows)  { [void]$sb.AppendLine("  win  $w") }
    [void]$sb.AppendLine("  fgnd $($s.Foreground)")
    foreach ($l in $s.Status)   { [void]$sb.AppendLine("  ctl  $l") }
    foreach ($l in $s.Sessions) { [void]$sb.AppendLine("  sess $l") }
    return $sb.ToString()
}

# --- main loop -------------------------------------------------------------

$buffer   = New-Object System.Collections.Generic.Queue[object]
$prev     = $null
$flat     = 0
$dumped   = $false

Write-Host "hydra-blackbox: seat $Seat, ${IntervalSec}s interval, stall after $StallSamples flat samples." -ForegroundColor Cyan
Write-Host "  stream -> $stream"
Write-Host "  dumps  -> $LogDir\STALL-*.txt"
Write-Host "  Ctrl-C to stop.`n"

while ($true) {
    $s = Get-Sample
    $text = Format-Sample $s

    Add-Content -Path $stream -Value $text -Encoding UTF8
    $buffer.Enqueue($s)
    while ($buffer.Count -gt $BufferSamples) { [void]$buffer.Dequeue() }

    # Stall signal: mirror burned no measurable CPU AND the capture log did
    # not grow. mirror is the consumer of the pixel ring; if it is idle the
    # ring is not being fed. Either half alone is too noisy.
    if ($prev) {
        $mNow  = $s.Procs['mirror']
        $mPrev = $prev.Procs['mirror']
        $cpuFlat = $mNow -and $mPrev -and (($mNow.CPU - $mPrev.CPU) -lt 0.01)
        $logFlat = ($s.CapLen -eq $prev.CapLen)
        $gone    = (-not $mNow) -or (-not $s.Procs['sdl-freerdp'] -and -not $s.Procs['hydrardp'])

        if ($gone) {
            $flat = $StallSamples   # a dead process is an instant stall
        } elseif ($cpuFlat -and $logFlat) {
            $flat++
        } else {
            if ($flat -gt 0) { Write-Verbose "recovered after $flat flat samples" }
            $flat   = 0
            $dumped = $false
        }
    }

    if ($flat -ge $StallSamples -and -not $dumped) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $out   = Join-Path $LogDir "STALL-$stamp.txt"
        $head  = @(
            "HYDRA STALL SNAPSHOT $stamp",
            "seat=$Seat interval=${IntervalSec}s flat_samples=$flat buffered=$($buffer.Count)",
            "Signal: mirror CPU flat AND capture_$Seat.log not growing (or a process vanished).",
            "",
            "Read the LAST few samples first, then walk backwards for the change.",
            "  - caplog stopped growing but mirror still burning CPU -> capture side",
            "  - both flat, sdl-freerdp iconic=True or covered      -> client suppression",
            "  - sdl-freerdp ABSENT / resp=False                    -> client died",
            "  - hydractl status errors, sess shows Disc            -> RDP stack wedged",
            "  - everything flat but seat was genuinely idle        -> not a fault",
            ("=" * 78), ""
        )
        Set-Content -Path $out -Value $head -Encoding UTF8
        foreach ($b in $buffer) { Add-Content -Path $out -Value (Format-Sample $b) -Encoding UTF8 }
        $dumped = $true
        Write-Host "[$(Get-Date -Format 'HH:mm:ss')] STALL -> $out" -ForegroundColor Red
    }

    $prev = $s
    Start-Sleep -Seconds $IntervalSec
}
