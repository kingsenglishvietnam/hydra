Set-StrictMode -Version Latest

# Pure planning/identity helpers. Importing this module starts no processes.
function ConvertTo-HydraArgument([AllowEmptyString()][string]$Value) {
    # Windows CommandLineToArgvW/CRT quoting, including terminal backslashes.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Get-HydraWindowsMonitors([string]$Text) {
    $pattern = '(?m)^\s*monitor\s+\d+:\s*\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)\s+(\\\\\.\\DISPLAY\d+)\s*$'
    foreach ($m in [regex]::Matches($Text, $pattern)) {
        [pscustomobject]@{
            Device = $m.Groups[5].Value
            X = [int]$m.Groups[1].Value; Y = [int]$m.Groups[2].Value
            Width = [int]$m.Groups[3].Value - [int]$m.Groups[1].Value
            Height = [int]$m.Groups[4].Value - [int]$m.Groups[2].Value
        }
    }
}

function Get-HydraClientMonitors([string]$Text) {
    # SDL/FreeRDP IDs are NOT Windows DISPLAY numbers. Names can be duplicated.
    $pattern = '(?m)^\s*(\*)?\s*\[(\d+)\]\s+\[([^\]]+)\]\s+(\d+)x(\d+)\s+\+(-?\d+)\+(-?\d+)\s*$'
    foreach ($m in [regex]::Matches($Text, $pattern)) {
        [pscustomobject]@{
            Id = [int]$m.Groups[2].Value; Name = $m.Groups[3].Value
            Width = [int]$m.Groups[4].Value; Height = [int]$m.Groups[5].Value
            X = [int]$m.Groups[6].Value; Y = [int]$m.Groups[7].Value
        }
    }
}

function New-HydraLocalPlan($Configuration, [string]$Seat, [string]$WindowsListing, [string]$ClientListing) {
    $selected = @($Configuration.seats | Where-Object { $_.name -ceq $Seat })
    if ($selected.Count -ne 1) { throw "Expected one configured seat named $Seat." }
    $s = $selected[0]
    if ($s.name -cnotmatch '^[A-Za-z0-9_-]{1,32}$') { throw 'Invalid seat name.' }
    if ($s.display_mode -ne 'off') { throw 'Local fullscreen clients require display_mode=off.' }
    if ($s.session -cnotmatch '^user:([^\r\n]+)$') { throw 'Local clients require an explicit user:NAME session.' }
    $user = $Matches[1]
    if ($s.monitor -eq $Configuration.console_monitor) { throw 'A remote seat cannot use the console monitor.' }
    $windows = @(Get-HydraWindowsMonitors $WindowsListing | Where-Object { $_.Device -eq $s.monitor })
    if ($windows.Count -ne 1) { throw "Configured display $($s.monitor) is not uniquely connected." }
    $w = $windows[0]
    $clients = @(Get-HydraClientMonitors $ClientListing | Where-Object {
        $_.X -eq $w.X -and $_.Y -eq $w.Y -and $_.Width -eq $w.Width -and $_.Height -eq $w.Height
    })
    if ($clients.Count -ne 1) { throw "Cannot uniquely map $($s.monitor) to a FreeRDP display; check DPI/topology." }
    $arguments = @('/v:127.0.0.2', "/u:$user", '/d:.', '/cert:tofu', '/sound',
                   '-suppress-output', '/network:lan', '/gfx:rfxc', '/f',
                   "/monitors:$($clients[0].Id)", "/t:Hydra seat $Seat")
    [pscustomobject]@{
        Seat = $Seat; User = $user; Display = $w.Device; ClientMonitorId = $clients[0].Id
        Arguments = $arguments
        CommandLine = (($arguments | ForEach-Object { ConvertTo-HydraArgument $_ }) -join ' ')
    }
}

function Test-HydraClientIdentity($Saved, $Current, [string]$ExpectedPath, [int]$ConsoleSession) {
    if ($null -eq $Saved -or $null -eq $Current) { return $false }
    return $Saved.Id -eq $Current.Id -and
           [string]$Saved.StartUtcTicks -ceq [string]$Current.StartUtcTicks -and
           $Saved.Path -eq $ExpectedPath -and $Current.Path -eq $ExpectedPath -and
           $Saved.ConsoleSession -eq $ConsoleSession -and $Current.ConsoleSession -eq $ConsoleSession
}

Export-ModuleMember -Function ConvertTo-HydraArgument, Get-HydraWindowsMonitors, Get-HydraClientMonitors, New-HydraLocalPlan, Test-HydraClientIdentity
