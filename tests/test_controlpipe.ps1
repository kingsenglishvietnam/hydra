$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'dist\tests'
$exe = Join-Path $out 'controlpipe_client.exe'
& cl.exe /nologo /MT /EHsc /std:c++17 "/Fo:$out\controlpipe_client.obj" "/Fe:$exe" (Join-Path $PSScriptRoot 'fixtures\controlpipe_client.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Failed to compile the actual CLI with its test-only pipe name.' }

$pipe = $null; $process = $null
try {
    $pipe = [IO.Pipes.NamedPipeServerStream]::new('hydra_test_cli_fragments',
        [IO.Pipes.PipeDirection]::InOut, 1, [IO.Pipes.PipeTransmissionMode]::Message,
        [IO.Pipes.PipeOptions]::Asynchronous)
    $pending = $pipe.BeginWaitForConnection($null, $null)
    $info = [Diagnostics.ProcessStartInfo]::new($exe, 'live-config')
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $pending.AsyncWaitHandle.WaitOne(5000)) { throw 'CLI did not connect.' }
    $pipe.EndWaitForConnection($pending)
    $request = New-Object byte[] 512
    $count = $pipe.Read($request, 0, $request.Length)
    if ([Text.Encoding]::Unicode.GetString($request, 0, $count) -cne 'live-config') { throw 'Unexpected CLI request.' }
    $payload = '{"padding":"' + ('x' * 12000) + '","seats":[{"name":"B"},{"name":"C"},{"name":"D"}]}'
    $bytes = [Text.Encoding]::Unicode.GetBytes($payload)
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
    $pipe.Dispose(); $pipe = $null
    if (-not $process.WaitForExit(5000)) { throw 'CLI did not finish reading the reply.' }
    if ($process.ExitCode -ne 0 -or $stdout.Result -cne $payload) {
        throw "CLI lost part of a multi-buffer message: $($stderr.Result)"
    }
    'PASS: actual CLI preserves a 12,000-character multi-buffer JSON reply'
} finally {
    if ($null -ne $pipe) { $pipe.Dispose() }
    if ($null -ne $process) {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
