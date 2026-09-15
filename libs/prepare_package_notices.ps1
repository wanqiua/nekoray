#requires -Version 7.2
# Generate notices from the packaged cores and their pinned dependency sources.
# DependencyRoot contains the downloaded Qt source ZIPs, cpp-sources and the
# OpenSSL license. Use a new output directory for every run to avoid stale files.
param(
    [ValidateNotNullOrEmpty()]
    [string]$DependencyRoot = (Join-Path $PSScriptRoot '../build/core-upgrade-check'),
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()]
    [string]$NoticesDirectory,
    [ValidateNotNullOrEmpty()]
    [string]$GoPath = 'C:/Program Files/Go/bin/go.exe'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DependencyRoot = (Resolve-Path -LiteralPath $DependencyRoot).Path
if (!(Test-Path -LiteralPath $DependencyRoot -PathType Container)) {
    throw 'DependencyRoot must be a directory containing the pinned dependency sources.'
}
$go = (Resolve-Path -LiteralPath $GoPath).Path
if (!(Test-Path -LiteralPath $go -PathType Leaf)) { throw 'GoPath must identify the Go executable.' }
$outputProvider = $null
$outputDrive = $null
$notices = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
    $NoticesDirectory, [ref]$outputProvider, [ref]$outputDrive)
if ($outputProvider.Name -ne 'FileSystem') { throw 'NoticesDirectory must be a filesystem path.' }
if (Test-Path -LiteralPath $notices) {
    throw "NoticesDirectory already exists; choose a new output directory: $notices"
}
# Omitting -Force also rejects an output created between the check and creation.
New-Item -ItemType Directory -Path $notices | Out-Null

function Is-Notice([string]$Path) {
    $name = [IO.Path]::GetFileName($Path)
    return $name -match '^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|AUTHORS|PATENTS|OFL)([._-]|$)' -or
        $Path -match '(^|[\\/])LICENSES[\\/]' -or $name -eq 'qt_attribution.json'
}
function Copy-Notices([string]$From, [string]$To) {
    $count = 0
    foreach ($file in Get-ChildItem -LiteralPath $From -Recurse -File -Force) {
        $relative = [IO.Path]::GetRelativePath($From, $file.FullName)
        if (!(Is-Notice $relative) -or $relative -match '(^|[\\/])\.git([\\/]|$)') { continue }
        $destination = Join-Path $To $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
        $count++
    }
    return $count
}
$cache = (& $go env GOMODCACHE).Trim()
$coreManifest = Get-Content -Raw -LiteralPath (Join-Path $repo 'deployment/windows64/core-versions.json') | ConvertFrom-Json
$modules = @{}
foreach ($core in @('nekoray_core', 'nekobox_core')) {
    $metadata = @(& $go version -m (Join-Path $repo "deployment/windows64/$core.exe"))
    if ($LASTEXITCODE -ne 0) { throw "Cannot read build metadata: $core" }
    (@("Go toolchain: $($coreManifest.cores.$core.go_version)") + @($metadata | Select-Object -Skip 1)) |
        Set-Content -LiteralPath (Join-Path $notices "$core-build-info.txt") -Encoding utf8
    $dependencies = [Collections.Generic.List[object]]::new()
    $dependency = $null
    foreach ($line in $metadata) {
        if ($line -match '^\s+dep\s+(\S+)\s+(\S+)') {
            $dependency = [pscustomobject]@{
                module = $Matches[1]; version = $Matches[2]
                source_module = $Matches[1]; source_version = $Matches[2]
            }
            $dependencies.Add($dependency)
        } elseif ($line -match '^\s+=>\s+(\S+)\s+(\S+)') {
            if (!$dependency) { throw 'Replacement without a preceding module.' }
            $dependency.source_module = $Matches[1]
            $dependency.source_version = $Matches[2]
        }
    }
    foreach ($dependency in $dependencies) {
        # The local shared RPC module is supplied in the accompanying source ZIP.
        if ($dependency.module -eq 'grpc_server') { continue }
        if ($dependency.source_version -notmatch '^v') { throw "Unpinned replacement: $($dependency.module)" }
        $key = "$($dependency.source_module)@$($dependency.source_version)"
        if ($modules.ContainsKey($key)) {
            if ($modules[$key].cores -notcontains $core) { $modules[$key].cores += $core }
            continue
        }
        $escaped = [regex]::Replace($key, '[A-Z]', {param($match) '!' + $match.Value.ToLowerInvariant()})
        $directory = Join-Path $cache $escaped
        if (!(Test-Path -LiteralPath $directory -PathType Container)) { throw "Missing source cache: $key" }
        $identifier = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($key))).Substring(0, 16).ToLowerInvariant()
        $relativeFolder = "go/$identifier"
        $noticeDirectory = Join-Path $notices $relativeFolder
        $count = Copy-Notices $directory $noticeDirectory
        $modules[$key] = [ordered]@{
            module = $dependency.module; version = $dependency.version
            source_module = $dependency.source_module; source_version = $dependency.source_version
            cores = @($core); notices = $relativeFolder; notice_files = $count
        }
        if ($count -eq 0) {
            if ($dependency.source_module -ne 'github.com/matsuridayo/libneko') {
                throw "No named license file found for $key"
            }
            New-Item -ItemType Directory -Path $noticeDirectory -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $directory 'README.md') -Destination $noticeDirectory
            @(
                "Source: https://github.com/MatsuriDayo/libneko/tree/5277a5bfc889"
                "Pinned module: $key"
                'This upstream snapshot contains no standalone LICENSE file.'
                'Its original README is included without assigning an additional license.'
                'The module checksum and exact dependency are recorded in the accompanying source.'
            ) | Set-Content -LiteralPath (Join-Path $noticeDirectory 'SOURCE-NOTICE.txt') -Encoding utf8
            $modules[$key].source_notice = 'Upstream snapshot has no standalone LICENSE; original README retained.'
        }
    }
}
@($modules.Values | Sort-Object { $_['source_module'] }, { $_['source_version'] }) |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $notices 'go-modules.json') -Encoding utf8
foreach ($entry in @(
    @('QHotkey', (Join-Path $repo '3rdparty/QHotkey')),
    @('qjs', (Join-Path $repo '3rdparty/qjs')),
    @('ZXing-2.0.0', (Join-Path $DependencyRoot 'cpp-sources/zxing-cpp-2.0.0')),
    @('yaml-cpp-0.7.0', (Join-Path $DependencyRoot 'cpp-sources/yaml-cpp-yaml-cpp-0.7.0')),
    @('protobuf-21.4', (Join-Path $DependencyRoot 'cpp-sources/protobuf-21.4'))
)) {
    $count = Copy-Notices $entry[1] (Join-Path $notices $entry[0])
    if ($count -eq 0) { throw "No notices found for $($entry[0])" }
}
$previousToolchain = $env:GOTOOLCHAIN
try {
    foreach ($version in ($coreManifest.cores.PSObject.Properties.Value.go_version | Sort-Object -Unique)) {
        $env:GOTOOLCHAIN = $version
        $toolchainRoot = (& $go env GOROOT).Trim()
        if ($LASTEXITCODE -ne 0) { throw "Cannot locate toolchain: $version" }
        Copy-Item -LiteralPath (Join-Path $toolchainRoot 'LICENSE') -Destination (Join-Path $notices "$version-LICENSE.txt")
        if (Test-Path -LiteralPath (Join-Path $toolchainRoot 'PATENTS')) {
            Copy-Item -LiteralPath (Join-Path $toolchainRoot 'PATENTS') -Destination (Join-Path $notices "$version-PATENTS.txt")
        }
    }
} finally { $env:GOTOOLCHAIN = $previousToolchain }
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($module in @('qtbase', 'qtsvg', 'qtimageformats')) {
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $DependencyRoot "$module-6.5.2-source.zip"))
    try {
        $prefix = Join-Path $notices "Qt-6.5.2/$module"
        $count = 0
        foreach ($entry in $archive.Entries) {
            if (!$entry.Name) { continue }
            $relative = ($entry.FullName -split '/', 2)[1]
            if (!(Is-Notice $relative)) { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $prefix $relative))
            if (!$target.StartsWith($prefix + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Unsafe archive path.'
            }
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            $count++
        }
        Write-Host "$module notices: $count"
    } finally { $archive.Dispose() }
}
Copy-Item -LiteralPath (Join-Path $DependencyRoot 'OpenSSL-3.0.10-LICENSE.txt') -Destination $notices
@'
NekoRay_BY_WanQiu is based on Nekoray 3.26 with Matsuri Xray 1.8.4 compatibility and sing-box 1.14.0.
Xray source: https://github.com/MatsuriDayo/Xray-core/tree/01208225ee7e508044cca8eb6776a117bcecd997
The compatibility core retains RC4-MD5 and allowInsecure from the original Nekoray fork.
Nekoray license: ../LICENSE (GPL-3.0).
Corresponding modified GUI/core adapter source is provided in the accompanying *-source.zip.

Qt 6.5.2 (Core, Gui, Widgets, Network, Svg and plugins): LGPL-3.0 / GPL alternatives as specified in the attached notices.
Source: https://github.com/qt/qtbase/tree/v6.5.2
        https://github.com/qt/qtsvg/tree/v6.5.2
        https://github.com/qt/qtimageformats/tree/v6.5.2
These libraries are dynamically linked and are replaceable by ABI-compatible builds.
Qt license texts and bundled third-party attributions are under Qt-6.5.2/.

OpenSSL 3.0.10: https://github.com/openssl/openssl/tree/openssl-3.0.10
Go 1.21.13 (Xray) and Go 1.26.4 (sing-box): https://go.dev/dl/
The exact linked Go modules, replacement sources and matching license directories are in go-modules.json.
Build metadata from each executable is also included. Module checksums are in the supplied source go.sum files.

C++ libraries: QHotkey 52e25acf221e5ac86ce648f6922620fb2d6a7121,
qjs 3fb0770c9ee5ab031a56072520641d4577b694e3, ZXing-C++ 2.0.0,
yaml-cpp 0.7.0, protobuf 21.4. Their notices are in named directories.
Additional in-tree third-party code and its original copyright headers are preserved in the source archive.

Microsoft Visual C++ x64 runtime: unmodified app-local files from Visual Studio Build Tools 14.51.36231 redist.
https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files

GeoIP/Geosite data: unmodified from the official Nekoray 3.26 Windows portable release.
https://github.com/MatsuriDayo/nekoray/releases/tag/3.26
'@ | Set-Content -LiteralPath (Join-Path $notices 'README.txt') -Encoding utf8
Write-Host "Linked Go module versions: $($modules.Count)"
Write-Host "Notice files: $(@(Get-ChildItem -LiteralPath $notices -Recurse -File).Count)"
