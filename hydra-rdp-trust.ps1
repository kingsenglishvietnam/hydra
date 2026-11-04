# hydra-rdp-trust.ps1 -- ONCE: let hydra7-avc's .rdp open without the
# Connect/Cancel security dialog.
#
# WHY. Since the April 2026 security update, mstsc shows a security dialog every
# time it opens an .rdp file, Cancel first for an unsigned one. A file signed by
# a certificate this machine lists as a trusted .rdp publisher opens without it.
# This makes that certificate and lists it; hydra7-avc.ps1 then signs its fresh
# .rdp with it on every launch.
#
# WHAT. A self-signed code-signing certificate, 'CN=Hydra rdp signing'. Its
# private key stays in the machine store, not exportable, so only an admin on
# this machine can sign with it. The public half goes into this machine's
# Trusted Root and Trusted Publishers stores -- the signature has to chain to a
# root -- and its thumbprints into the trusted-publisher policy,
#   HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services
#     TrustedCertThumbprints
# appended to whatever is listed there already. The policy says SHA1; some
# builds read SHA256. Both go in: a string that matches nothing is ignored.
#
# THE PRICE: this machine trusts code signed with that key. The key exists only
# here, behind admin rights. -Undo takes all of it back out.
#
# USAGE (elevated, from the Hydra Shell)
#   .\hydra-rdp-trust.ps1
#   .\hydra-rdp-trust.ps1 -Undo

param([switch]$Undo)

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say 'Run this elevated, from the Hydra Shell: it writes to the machine stores and policy.' Red
    return
}

$subject = 'CN=Hydra rdp signing'                  # hydra7-avc.ps1 looks for exactly this
$polKey  = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services'
$polName = 'TrustedCertThumbprints'

function Get-Sha256($cert) {
    -join ([Security.Cryptography.SHA256]::Create().ComputeHash($cert.RawData) |
        ForEach-Object { $_.ToString('X2') })
}
function Get-Trusted {
    $v = (Get-ItemProperty -Path $polKey -Name $polName -EA SilentlyContinue).$polName
    @($v -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
function Set-Trusted([string[]]$List) {
    if ($List) {
        if (-not (Test-Path $polKey)) { New-Item -Path $polKey -Force | Out-Null }
        New-ItemProperty -Path $polKey -Name $polName -Value ($List -join ',') `
            -PropertyType String -Force | Out-Null
    } else {
        Remove-ItemProperty -Path $polKey -Name $polName -EA SilentlyContinue
    }
}
function Get-Mine { @(Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $subject) }
function Remove-FromStore([string]$Store, [string]$Thumb) {
    $s = New-Object Security.Cryptography.X509Certificates.X509Store($Store, 'LocalMachine')
    $s.Open('ReadWrite')
    foreach ($c in @($s.Certificates.Find('FindByThumbprint', $Thumb, $false))) { $s.Remove($c) }
    $s.Close()
}
function Add-ToStore([string]$Store, [byte[]]$Raw) {
    $s = New-Object Security.Cryptography.X509Certificates.X509Store($Store, 'LocalMachine')
    $s.Open('ReadWrite')
    $s.Add((New-Object Security.Cryptography.X509Certificates.X509Certificate2(, $Raw)))   # public half only
    $s.Close()
}

# ----------------------------------------------------------------- undo -----
if ($Undo) {
    $mine = Get-Mine
    if (-not $mine) { Say 'no Hydra rdp-signing certificate on this machine.' DarkGray }
    $drop = @()
    foreach ($c in $mine) { $drop += $c.Thumbprint; $drop += (Get-Sha256 $c) }
    Set-Trusted @(Get-Trusted | Where-Object { $drop -notcontains $_ })
    foreach ($c in $mine) {
        Remove-FromStore 'Root' $c.Thumbprint
        Remove-FromStore 'TrustedPublisher' $c.Thumbprint
        try   { Remove-Item -Path "Cert:\LocalMachine\My\$($c.Thumbprint)" -DeleteKey -ErrorAction Stop }
        catch { Remove-FromStore 'My' $c.Thumbprint }
    }
    Say 'removed: the certificate, its trust and its policy entries. mstsc asks Connect/Cancel again.' Green
    return
}

# ----------------------------------------------------------------- make -----
$cert = Get-Mine | Where-Object HasPrivateKey | Select-Object -First 1
if ($cert) {
    Say "certificate already here: $($cert.Thumbprint)" DarkGray
} else {
    # Through Windows PowerShell on purpose: under pwsh 7 the PKI module can run
    # in a compatibility session and hand back a lifeless copy. The store is
    # what counts, so the certificate is read back from it below.
    $ps51 = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $ps51 -NoProfile -Command ("New-SelfSignedCertificate -Type CodeSigningCert -Subject '$subject' " +
        "-CertStoreLocation Cert:\LocalMachine\My -KeyExportPolicy NonExportable " +
        "-KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 " +
        "-NotAfter (Get-Date).AddYears(20) | Out-Null")
    $cert = Get-Mine | Where-Object HasPrivateKey | Select-Object -First 1
    if (-not $cert) { Say 'could not create the certificate -- see the error above.' Red; return }
    Say "certificate made: $($cert.Thumbprint)" Green
}

foreach ($store in 'Root', 'TrustedPublisher') {
    Remove-FromStore $store $cert.Thumbprint       # a rerun replaces rather than duplicates
    Add-ToStore $store $cert.RawData
}
$list = @(Get-Trusted)
foreach ($t in $cert.Thumbprint, (Get-Sha256 $cert)) { if ($list -notcontains $t) { $list += $t } }
Set-Trusted $list
Say 'trusted on this machine as an .rdp publisher.' Green
Say 'The next hydra7-avc.ps1 launch signs its .rdp, and mstsc connects without asking.' Cyan
