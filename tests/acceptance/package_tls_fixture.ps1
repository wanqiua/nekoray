param(
    [Parameter(Mandatory)][string]$Certificate,
    [Parameter(Mandatory)][string]$PrivateKey,
    [ValidateSet('localhost', 'mismatch.example.test')][string]$DnsName = 'localhost'
)

$ErrorActionPreference = 'Stop'
$rsa = [System.Security.Cryptography.RSA]::Create(2048)
$certificateObject = $null
try {
    $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        "CN=$DnsName", $rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $san = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
    $san.AddDnsName($DnsName)
    $request.CertificateExtensions.Add($san.Build())
    $request.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))
    $usage = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor
        [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment -bor
        [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign
    $request.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new($usage, $true))
    $certificateObject = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddDays(2))
    $encoding = [System.Text.UTF8Encoding]::new($false)
    # CreateSelfSigned and these PEM exports do not open or modify a certificate store.
    [System.IO.File]::WriteAllText($Certificate, $certificateObject.ExportCertificatePem(), $encoding)
    [System.IO.File]::WriteAllText($PrivateKey, $rsa.ExportRSAPrivateKeyPem(), $encoding)
} finally {
    if ($null -ne $certificateObject) { $certificateObject.Dispose() }
    $rsa.Dispose()
}
