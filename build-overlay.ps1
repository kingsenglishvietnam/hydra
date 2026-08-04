#requires -RunAsAdministrator
# build-overlay.ps1 -- build the cursor overlay AS A UIAccess app so its cursor
# draws above the Start menu / system UI.
#
# The four UIAccess conditions, handled here:
#   1. manifest with uiAccess="true"  -> embedded via mt.exe (or /MANIFEST link)
#   2. Authenticode-signed            -> signed with your HydraTest cert
#   3. trusted install path           -> copied to C:\Program Files\Hydra\
#   4. launched elevated              -> hydrad (SYSTEM) launches it; or run elevated
#
# Run from an ELEVATED x64 Native Tools prompt -> powershell, in the hydra folder:
#   .\build-overlay.ps1
#
# WHY EACH STEP: UIAccess fails SILENTLY if any condition is off -- the window
# just won't rise above Start, with no error. So this script verifies each one.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe not on PATH. Use an x64 Native Tools prompt, then 'powershell'."
}

$src      = Join-Path $root 'overlay\cursor_overlay.cpp'
$manifest = Join-Path $root 'overlay\cursor_overlay.manifest'
$exe      = Join-Path $dist 'cursor_overlay.exe'

# --- 1. compile + link WITHOUT an auto-manifest, so we embed ours cleanly ---
Write-Host "compiling cursor_overlay ..." -ForegroundColor Cyan
# /MANIFEST:NO stops the linker adding a default manifest that would fight ours.
& cl.exe /nologo /O2 /EHsc "$src" /Fo:"$dist\" /Fe:"$exe" `
    /link gdi32.lib user32.lib advapi32.lib gdiplus.lib /MANIFEST:NO
if ($LASTEXITCODE -ne 0) { Write-Error "compile/link failed ($LASTEXITCODE)" }
Write-Host "  built $exe" -ForegroundColor Green

# --- 2. embed the UIAccess manifest with mt.exe ---
$mt = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter mt.exe -ErrorAction SilentlyContinue |
      Where-Object FullName -match 'x64' | Select-Object -Last 1
if (-not $mt) { Write-Error "mt.exe not found (Windows SDK). Can't embed the manifest." }
& $mt.FullName -nologo -manifest "$manifest" -outputresource:"$exe;#1"
if ($LASTEXITCODE -ne 0) { Write-Error "mt.exe manifest embed failed ($LASTEXITCODE)" }
Write-Host "  embedded UIAccess manifest" -ForegroundColor Green

# --- 3. sign the exe (UIAccess REQUIRES Authenticode) with the HydraTest cert ---
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=HydraTest' } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=HydraTest" `
            -CertStoreLocation Cert:\CurrentUser\My
    Write-Host "  created HydraTest cert" -ForegroundColor Green
}
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object FullName -match 'x64' | Select-Object -Last 1
if (-not $signtool) { Write-Error "signtool.exe not found (Windows SDK)." }
& $signtool.FullName sign /fd sha256 /sha1 $cert.Thumbprint "$exe"
if ($LASTEXITCODE -ne 0) { Write-Error "signtool failed ($LASTEXITCODE)" }
Write-Host "  signed (Authenticode)" -ForegroundColor Green

# --- ensure the cert is trusted so the signature validates for UIAccess ---
$pw = ConvertTo-SecureString "hydra" -AsPlainText -Force
$pfx = Join-Path $env:TEMP 'hydratest_overlay.pfx'
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
Import-PfxCertificate -FilePath $pfx -Password $pw -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-PfxCertificate -FilePath $pfx -Password $pw -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Remove-Item $pfx -Force
Write-Host "  cert trusted (Root + TrustedPublisher)" -ForegroundColor Green

# --- 4. install to a TRUSTED PATH (UIAccess refuses to run from untrusted dirs) ---
$installDir = Join-Path ${env:ProgramFiles} 'Hydra'
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
$installed = Join-Path $installDir 'cursor_overlay.exe'
Copy-Item "$exe" $installed -Force
# gdiplus is a system DLL, no need to copy. Copy the signed exe only.
Write-Host "  installed to $installed (trusted path)" -ForegroundColor Green

Write-Host ""
Write-Host "UIAccess overlay ready." -ForegroundColor Cyan
Write-Host "  RUN IT FROM THE TRUSTED PATH, ELEVATED:" -ForegroundColor Yellow
Write-Host "    & '$installed' 4" -ForegroundColor Yellow
Write-Host ""
Write-Host "  (Running the copy in dist\ will NOT get UIAccess -- only the one in" -ForegroundColor DarkYellow
Write-Host "   Program Files\Hydra is in a trusted path. That distinction is the" -ForegroundColor DarkYellow
Write-Host "   thing that silently breaks UIAccess if you forget it.)" -ForegroundColor DarkYellow
Write-Host ""
Write-Host "  Verify UIAccess actually took:" -ForegroundColor Cyan
Write-Host "    open Start while it's running -- the cursor should now draw over it."
