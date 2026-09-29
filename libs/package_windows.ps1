#requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$GuiPath,
    [Parameter(Mandatory)][string]$QtSdk,
    [Parameter(Mandatory)][string]$CrtDirectory,
    [Parameter(Mandatory)][string]$GeoDataDirectory,
    [Parameter(Mandatory)][string]$NoticesDirectory,
    [string]$CoreDirectory = (Join-Path $PSScriptRoot '../deployment/windows64'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../deployment/packages'),
    [string]$SevenZip = 'C:/Program Files/7-Zip/7z.exe',
    [switch]$ValidateInputsOnly,
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$PackageName = ('NekoRay_BY_WanQiu-' + (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '../nekoray_version.txt')).Trim() + '-windows-x64')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_source_inventory.ps1')
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$productName = 'NekoRay_BY_WanQiu'
$guiVersion = (Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'nekoray_version.txt')).Trim()
$guiFilename = "$productName.exe"
$GuiPath = (Resolve-Path -LiteralPath $GuiPath).Path
$QtSdk = (Resolve-Path -LiteralPath $QtSdk).Path
$CoreDirectory = (Resolve-Path -LiteralPath $CoreDirectory).Path
$CrtDirectory = (Resolve-Path -LiteralPath $CrtDirectory).Path
$GeoDataDirectory = (Resolve-Path -LiteralPath $GeoDataDirectory).Path
$NoticesDirectory = (Resolve-Path -LiteralPath $NoticesDirectory).Path
$SevenZip = (Resolve-Path -LiteralPath $SevenZip).Path
$windeploy = Join-Path $QtSdk 'bin/windeployqt.exe'
$coreManifest = Get-Content -Raw -LiteralPath (Join-Path $CoreDirectory 'core-versions.json') | ConvertFrom-Json
if ($coreManifest.product -ne $productName -or $coreManifest.gui_version -ne $guiVersion) {
    throw 'Rebuild both cores for the current product version before packaging.'
}
$guiInfo = (Get-Item -LiteralPath $GuiPath).VersionInfo
if ($guiInfo.ProductName -ne $productName -or $guiInfo.ProductVersion -ne "$guiVersion.0.0") {
    throw 'The GUI product name/version does not match the current release.'
}
if ($coreManifest.target_os -ne 'windows' -or $coreManifest.target_arch -ne 'amd64') {
    throw 'The core manifest must describe Windows amd64 binaries.'
}
if ($coreManifest.gui_base -ne '3.26' -or
    $coreManifest.cores.nekoray_core.version -ne 'v1.260327.0' -or
    $coreManifest.cores.nekoray_core.source_module -ne 'github.com/wanqiua/Xray-core' -or
    $coreManifest.cores.nekoray_core.source_version -ne 'v0.0.0-20260928091204-e9639bd461dc' -or
    $coreManifest.cores.nekoray_core.source_revision -ne 'e9639bd461dcff1c91e26a71a8752fa42d51268d' -or
    $coreManifest.cores.nekobox_core.version -ne 'v1.14.0' -or
    $coreManifest.cores.nekobox_core.source_module -ne 'github.com/sagernet/sing-box') {
    throw 'Package inputs must use the pinned wanqiua Xray 26.3.27 compatibility source and sing-box 1.14.0.'
}
foreach ($core in @('nekoray_core', 'nekobox_core')) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $CoreDirectory "$core.exe") -Algorithm SHA256).Hash
    if ($actual -ne $coreManifest.cores.$core.sha256) { throw "Core hash mismatch: $core" }
}
$requiredInputs = @($windeploy, $GuiPath, $SevenZip, (Join-Path $NoticesDirectory 'README.txt'))
$requiredInputs += @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') | ForEach-Object { Join-Path $CrtDirectory $_ }
$requiredInputs += @('Qt6Core.dll', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll') | ForEach-Object { Join-Path $QtSdk "bin/$_" }
$requiredInputs += @('geoip.dat', 'geosite.dat', 'geoip.db', 'geosite.db') | ForEach-Object { Join-Path $GeoDataDirectory $_ }
$packageDocuments = @('Core_Upgrade.md', 'Build_Core.md', 'Build_Windows.md', 'Dynamic_Core_Implementation.md', 'Release_0.1.md', 'Release_0.11.md', 'Release_0.2.md', 'TLS_Certificate_Pins.md', 'Versioning.md')
$requiredInputs += @('Portable_Windows.md') + $packageDocuments | ForEach-Object { Join-Path $projectRoot "docs/$_" }
$requiredInputs += @('nekoray.png', 'nekobox.png', 'qtbase_zh_CN.qm') | ForEach-Object { Join-Path $projectRoot "res/public/$_" }
$requiredInputs += Join-Path $projectRoot 'LICENSE'
foreach ($required in $requiredInputs) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing package input: $required" }
}
$submoduleRevisions = [ordered]@{}
foreach ($submodule in @('3rdparty/QHotkey', '3rdparty/qjs')) {
    $entry = (& git -C $projectRoot ls-files --stage -- $submodule | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $entry -notmatch '^160000 ([0-9a-f]{40}) 0\s') {
        throw "Cannot identify the pinned submodule: $submodule"
    }
    $submoduleRevisions[[IO.Path]::GetFileName($submodule)] = $Matches[1]
    if (!(Test-Path -LiteralPath (Join-Path $projectRoot "$submodule/CMakeLists.txt") -PathType Leaf)) {
        throw "Missing submodule source: $submodule"
    }
}
$sourceFiles = @(Get-PackageSourceInventory -ProjectRoot $projectRoot)
if ($ValidateInputsOnly) {
    Write-Host "Package inputs, pinned core sources, core hashes and $($sourceFiles.Count) source paths verified. No package or staging directory was created."
    return
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$portableZip = Join-Path $OutputDirectory "$PackageName.zip"
$sourceZipName = "$PackageName-source.zip"
$sourceZip = Join-Path $OutputDirectory $sourceZipName
$checksumFile = Join-Path $OutputDirectory "$PackageName.sha256"
foreach ($output in @($portableZip, $sourceZip, $checksumFile)) {
    if (Test-Path -LiteralPath $output) { throw "Output already exists; use another PackageName: $output" }
}
# Every run gets a fresh directory. Existing deployments and user data are never copied wholesale.
$work = Join-Path $OutputDirectory ('.stage-' + [Guid]::NewGuid().ToString('N'))
$portable = Join-Path $work "portable/$productName"
$source = Join-Path $work "source/$productName"
New-Item -ItemType Directory -Path $portable, $source -Force | Out-Null

function Copy-Payload([string]$From, [string]$Relative) {
    $target = Join-Path $portable $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $From -Destination $target
}

Copy-Payload $GuiPath $guiFilename
foreach ($name in @('nekoray_core.exe', 'nekobox_core.exe', 'core-versions.json')) {
    Copy-Payload (Join-Path $CoreDirectory $name) $name
}
$oldPath = $env:PATH
try {
    $env:PATH = (Join-Path $QtSdk 'bin') + ';' + $oldPath
    & $windeploy --release --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw --no-translations --no-pdf (Join-Path $portable $guiFilename)
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }
} finally { $env:PATH = $oldPath }
foreach ($name in @('libcrypto-3-x64.dll', 'libssl-3-x64.dll')) {
    Copy-Payload (Join-Path $QtSdk "bin/$name") $name
}
foreach ($file in Get-ChildItem -LiteralPath $CrtDirectory -File -Filter '*.dll') {
    Copy-Payload $file.FullName $file.Name
}
foreach ($name in @('geoip.dat', 'geosite.dat', 'geoip.db', 'geosite.db')) {
    Copy-Payload (Join-Path $GeoDataDirectory $name) $name
}
foreach ($name in @('nekoray.png', 'nekobox.png', 'qtbase_zh_CN.qm')) {
    Copy-Payload (Join-Path $projectRoot "res/public/$name") $name
}
Copy-Payload (Join-Path $projectRoot 'LICENSE') 'LICENSE'
Copy-Payload (Join-Path $projectRoot 'docs/Portable_Windows.md') 'README.md'
foreach ($name in $packageDocuments) {
    Copy-Payload (Join-Path $projectRoot "docs/$name") "docs/$name"
}
Copy-Item -LiteralPath $NoticesDirectory -Destination (Join-Path $portable 'licenses') -Recurse
"[Paths]`nPrefix=.`nPlugins=.`nTranslations=.`n" | Set-Content -LiteralPath (Join-Path $portable 'qt.conf') -Encoding utf8

# Capture the current source files, including untracked implementation files.
# Recheck each path when copying so runtime directories and links cannot enter
# through an ignored-file exception or a changed submodule directory.
foreach ($relative in $sourceFiles) {
    $inputPath = Resolve-PackageSourceFile -ProjectRoot $projectRoot -Relative $relative
    if ($null -eq $inputPath) { continue }
    $target = Join-Path $source $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $inputPath -Destination $target
}
$baseRevision = (& git -C $projectRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not read the source revision.' }
$sourceInfo = [ordered]@{
    upstream = 'https://github.com/MatsuriDayo/nekoray'
    base_revision = $baseRevision
    includes_working_tree_changes = $true
    captured_at_utc = [DateTime]::UtcNow.ToString('o')
    submodules = $submoduleRevisions
}
$sourceInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $source 'BUILD-SOURCE.json') -Encoding utf8

function New-VerifiedZip([string]$Directory, [string]$Destination) {
    Push-Location $Directory
    try {
        & $SevenZip a -tzip -mx=7 -mmt=on $Destination $productName -bso0 -bsp0
        if ($LASTEXITCODE -ne 0) { throw "Could not create ZIP: $Destination" }
        & $SevenZip t $Destination -bso0 -bsp0
        if ($LASTEXITCODE -ne 0) { throw "ZIP integrity check failed: $Destination" }
    } finally { Pop-Location }
}
New-VerifiedZip (Split-Path -Parent $source) $sourceZip

$fileManifest = @(Get-ChildItem -LiteralPath $portable -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = [IO.Path]::GetRelativePath($portable, $_.FullName).Replace('\', '/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$manifest = [ordered]@{
    product = $productName
    gui_version = $guiVersion
    gui_executable = $guiFilename
    package = $PackageName
    platform = 'Windows 10/11 x64'
    packaged_at_utc = [DateTime]::UtcNow.ToString('o')
    gui_base = $coreManifest.gui_base
    xray = $coreManifest.cores.nekoray_core.version
    sing_box = $coreManifest.cores.nekobox_core.version
    qt = (Get-Item -LiteralPath (Join-Path $portable 'Qt6Core.dll')).VersionInfo.FileVersion
    openssl = (Get-Item -LiteralPath (Join-Path $portable 'libssl-3-x64.dll')).VersionInfo.FileVersion
    cores = $coreManifest.cores
    geodata_source = 'Unmodified data from the official Nekoray 3.26 Windows ZIP'
    source = @{
        archive = $sourceZipName
        sha256 = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash.ToLowerInvariant()
        base_revision = $baseRevision
        includes_working_tree_changes = $true
    }
    files = $fileManifest
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $portable 'package-manifest.json') -Encoding utf8
New-VerifiedZip (Split-Path -Parent $portable) $portableZip
@($portableZip, $sourceZip) | ForEach-Object {
    (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($_)
} | Set-Content -LiteralPath $checksumFile -Encoding ascii
Write-Host "Portable: $portableZip"
Write-Host "Source:   $sourceZip"
Write-Host "SHA256:   $checksumFile"
Write-Host "Staging:  $work"
