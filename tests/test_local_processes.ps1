# Exercise the actual StopClient entry path against two harmless child processes.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sandbox = Join-Path $root ('dist\tests\local-client-' + [Guid]::NewGuid().ToString('N'))
$bin = Join-Path $sandbox 'dist\freerdp'
$state = Join-Path $sandbox 'state\local-clients'
$client = Join-Path $bin 'sdl-freerdp.exe'
$children = @()
try {
    New-Item -ItemType Directory -Force -Path $bin, $state | Out-Null
    & cl.exe /nologo /MT "/Fo:$bin\client.obj" "/Fe:$client" (Join-Path $PSScriptRoot 'fixtures\client_stub.c')
    if ($LASTEXITCODE -ne 0) { throw 'Failed to compile the inert client fixture.' }
    foreach ($seat in @('B', 'C')) {
        $child = Start-Process -FilePath $client -PassThru -WindowStyle Hidden
        $children += $child
        $null = $child.Handle
        $record = [pscustomobject]@{
            Id = $child.Id; StartUtcTicks = [string]$child.StartTime.ToUniversalTime().Ticks
            Path = $client; ConsoleSession = $child.SessionId; Seat = $seat
        }
        [IO.File]::WriteAllText((Join-Path $state "$seat.json"), ($record | ConvertTo-Json))
    }
    & (Join-Path $root 'hydra-local.ps1') -Root $sandbox -Seat B -StopClient
    $children[0].Refresh(); $children[1].Refresh()
    if (-not $children[0].HasExited -or $children[1].HasExited) {
        throw 'Closing B affected the wrong client.'
    }
    'PASS: real StopClient terminates B while C keeps running'

    # A stale B record points to C's PID, with a different creation time.
    $wrong = [pscustomobject]@{
        Id = $children[1].Id; StartUtcTicks = '1'; Path = $client
        ConsoleSession = $children[1].SessionId; Seat = 'B'
    }
    [IO.File]::WriteAllText((Join-Path $state 'B.json'), ($wrong | ConvertTo-Json))
    $rejected = $false
    try { & (Join-Path $root 'hydra-local.ps1') -Root $sandbox -Seat B -StopClient }
    catch { $rejected = $true }
    $children[1].Refresh()
    if (-not $rejected -or $children[1].HasExited) { throw 'Stale record was not rejected safely.' }
    'PASS: real StopClient refuses a reused/mismatched process identity'

    & (Join-Path $root 'hydra-local.ps1') -Root $sandbox -Seat C -StopClient
    $children[1].Refresh()
    if (-not $children[1].HasExited) { throw 'C client was not closed.' }
    'PASS: C can subsequently close independently'
} finally {
    foreach ($child in $children) {
        try { if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() } }
        finally { $child.Dispose() }
    }
    if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force }
}
