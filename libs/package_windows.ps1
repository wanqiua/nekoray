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
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$PackageName = ('nekoray-3.26-dual-core-windows-x64-' + (Get-Date -Format yyyyMMdd))
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$GuiPath = (Resolve-Path -LiteralPath $GuiPath).Path
$QtSdk = (Resolve-Path -LiteralPath $QtSdk).Path
$CoreDirectory = (Resolve-Path -LiteralPath $CoreDirectory).Path
$CrtDirectory = (Resolve-Path -LiteralPath $CrtDirectory).Path
$GeoDataDirectory = (Resolve-Path -LiteralPath $GeoDataDirectory).Path
$NoticesDirectory = (Resolve-Path -LiteralPath $NoticesDirectory).Path
$SevenZip = (Resolve-Path -LiteralPath $SevenZip).Path
$windeploy = Join-Path $QtSdk 'bin/windeployqt.exe'
$coreManifest = Get-Content -Raw -LiteralPath (Join-Path $CoreDirectory 'core-versions.json') | ConvertFrom-Json
if ($coreManifest.target_os -ne 'windows' -or $coreManifest.target_arch -ne 'amd64') {
    throw 'The core manifest must describe Windows amd64 binaries.'
}
foreach ($core in @('nekoray_core', 'nekobox_core')) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $CoreDirectory "$core.exe") -Algorithm SHA256).Hash
    if ($actual -ne $coreManifest.cores.$core.sha256) { throw "Core hash mismatch: $core" }
}
foreach ($required in @($windeploy, (Join-Path $CrtDirectory 'vcruntime140.dll'),
        (Join-Path $NoticesDirectory 'README.txt'))) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing package input: $required" }
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
$portable = Join-Path $work 'portable/nekoray'
$source = Join-Path $work 'source/nekoray'
New-Item -ItemType Directory -Path $portable, $source -Force | Out-Null

function Copy-Payload([string]$From, [string]$Relative) {
    $target = Join-Path $portable $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $From -Destination $target
}

Copy-Payload $GuiPath 'nekoray.exe'
foreach ($name in @('nekoray_core.exe', 'nekobox_core.exe', 'core-versions.json')) {
    Copy-Payload (Join-Path $CoreDirectory $name) $name
}
$oldPath = $env:PATH
try {
    $env:PATH = (Join-Path $QtSdk 'bin') + ';' + $oldPath
    & $windeploy --release --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw --no-translations --no-pdf (Join-Path $portable 'nekoray.exe')
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
foreach ($name in @('Core_Upgrade.md', 'Build_Core.md', 'Build_Windows.md')) {
    Copy-Payload (Join-Path $projectRoot "docs/$name") "docs/$name"
}
Copy-Item -LiteralPath $NoticesDirectory -Destination (Join-Path $portable 'licenses') -Recurse
"[Paths]`nPrefix=.`nPlugins=.`nTranslations=.`n" | Set-Content -LiteralPath (Join-Path $portable 'qt.conf') -Encoding utf8

# Capture working files, including untracked implementation files and pinned submodules.
# git archive HEAD would omit all local modifications in this development build.
$gitFiles = @(& git -C $projectRoot -c core.quotepath=false ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate the source tree.' }
$sourceFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $gitFiles) { [void]$sourceFiles.Add($relative) }
foreach ($submodule in @('3rdparty/QHotkey', '3rdparty/qjs')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $projectRoot $submodule) -Recurse -File -Force) {
        $relative = [IO.Path]::GetRelativePath($projectRoot, $file.FullName).Replace('\', '/')
        if ($relative -notmatch '(^|/)\.git(/|$)') { [void]$sourceFiles.Add($relative) }
    }
}
foreach ($relative in ($sourceFiles | Sort-Object)) {
    $inputPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if (!$inputPath.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source path escapes the repository: $relative"
    }
    if (!(Test-Path -LiteralPath $inputPath -PathType Leaf)) { continue }
    if ((Get-Item -LiteralPath $inputPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Source links require manual review: $relative"
    }
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
    submodules = @{
        QHotkey = '52e25acf221e5ac86ce648f6922620fb2d6a7121'
        qjs = '3fb0770c9ee5ab031a56072520641d4577b694e3'
    }
}
$sourceInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $source 'BUILD-SOURCE.json') -Encoding utf8

function New-VerifiedZip([string]$Directory, [string]$Destination) {
    Push-Location $Directory
    try {
        & $SevenZip a -tzip -mx=7 -mmt=on $Destination 'nekoray' -bso0 -bsp0
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
