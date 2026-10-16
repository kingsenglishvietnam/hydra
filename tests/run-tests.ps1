# Compile and run native tests without installing services or input drivers.
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'An MSVC x64 C++ toolset is required.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$out = Join-Path $root 'dist\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($name in @('test_config.cpp', 'test_input_binding.c', 'test_mouse_buttons.c', 'test_edid.c')) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($name)
    $exe = Join-Path $out "$stem.exe"
    $flags = @('/nologo', '/W4', '/MT', "/Fo:$out\$stem.obj", "/Fe:$exe")
    if ($name.EndsWith('.cpp')) { $flags += @('/EHsc', '/std:c++17') }
    & cl.exe @flags (Join-Path $PSScriptRoot $name)
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
}
& (Join-Path $PSScriptRoot 'test_local_launcher.ps1')
& (Join-Path $PSScriptRoot 'test_local_processes.ps1')
& (Join-Path $PSScriptRoot 'test_controlpipe.ps1')
