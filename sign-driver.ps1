#requires -RunAsAdministrator
# sign-driver.ps1 -- create a test cert (once), sign a driver catalog, trust it.
#
# Takes the catalog NAME as well as the directory: there is more than one driver
# in this tree now (iddseat, hydrakbd), and hardcoding one of them meant the
# other silently failed to sign -- producing a .sys that builds cleanly and then
# refuses to load, which is a confusing way to find out.
# Run AFTER the WDK build produces dist\driver\iddseat.{dll,cat,inf}.
#   .\sign-driver.ps1 -DriverDir .\dist\driver
param(
    [string]$DriverDir = ".\dist\driver",
    [string]$CatalogName = ""      # default: whatever .cat is in $DriverDir
)

$ErrorActionPreference='Stop'
if ($CatalogName) {
    $cat = Join-Path $DriverDir $CatalogName
} else {
    # Exactly one .cat is the normal case; if there are several, be explicit
    # rather than guessing which one was meant.
    $cats = @(Get-ChildItem $DriverDir -Filter *.cat -ErrorAction SilentlyContinue)
    if ($cats.Count -eq 0) {
        Write-Error "no .cat in $DriverDir -- run inf2cat first (see BUILD.md 2)."
    } elseif ($cats.Count -gt 1) {
        Write-Error "several .cat files in $DriverDir -- pass -CatalogName to pick one: $($cats.Name -join ', ')"
    }
    $cat = $cats[0].FullName
}
if (-not (Test-Path $cat)) { Write-Error "catalog not found: $cat" }
Write-Host "  signing $(Split-Path $cat -Leaf)" -ForegroundColor DarkGray

# reuse an existing HydraTest cert or make one
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=HydraTest' } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=HydraTest" `
            -CertStoreLocation Cert:\CurrentUser\My
    Write-Host "created HydraTest code-signing cert" -ForegroundColor Green
} else { Write-Host "reusing existing HydraTest cert" -ForegroundColor Green }

# find signtool (newest SDK bin)
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'x64' } | Select-Object -Last 1
if (-not $signtool) { Write-Error "signtool.exe not found -- is the WDK/SDK installed?" }

& $signtool.FullName sign /fd sha256 /sha1 $cert.Thumbprint $cat
if ($LASTEXITCODE -ne 0) { Write-Error "signtool failed ($LASTEXITCODE)" }
Write-Host "signed $cat" -ForegroundColor Green

# trust the cert: LocalMachine Root + TrustedPublisher
$pw = ConvertTo-SecureString "hydra" -AsPlainText -Force
$pfx = Join-Path $env:TEMP 'hydratest.pfx'
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
Import-PfxCertificate -FilePath $pfx -Password $pw -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-PfxCertificate -FilePath $pfx -Password $pw -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Remove-Item $pfx -Force
Write-Host "cert trusted (Root + TrustedPublisher). Now: pnputil /add-driver $DriverDir\iddseat.inf /install" -ForegroundColor Cyan
