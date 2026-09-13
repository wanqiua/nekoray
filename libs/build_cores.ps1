param(
    [ValidateSet('windows', 'linux', 'darwin')]
    [string]$TargetOS = 'windows',
    [ValidateSet('amd64', 'arm64')]
    [string]$TargetArch = 'amd64',
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$destinationName = if ($TargetOS -eq 'windows' -and $TargetArch -eq 'amd64') { 'windows64' } elseif ($TargetOS -eq 'linux' -and $TargetArch -eq 'amd64') { 'linux64' } else { "$TargetOS-$TargetArch" }
$destination = Join-Path $projectRoot "deployment/$destinationName"
$extension = if ($TargetOS -eq 'windows') { '.exe' } else { '' }
$tags = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'core_build_tags.txt')).Trim()
$version = 'nekoray-' + (Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'nekoray_version.txt')).Trim() + '-dual-core'
$oldEnvironment = @{}
foreach ($name in @('CGO_ENABLED', 'GOOS', 'GOARCH', 'GOTOOLCHAIN')) {
    $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$manifest = [ordered]@{
    gui_base = '3.26'
    target_os = $TargetOS
    target_arch = $TargetArch
    built_at_utc = [DateTime]::UtcNow.ToString('o')
    cores = [ordered]@{}
}
try {
    if ($Test) {
        $hostOS = (& go env GOHOSTOS).Trim()
        $hostArch = (& go env GOHOSTARCH).Trim()
        if ($hostOS -ne $TargetOS -or $hostArch -ne $TargetArch) { throw 'Tests must run on the host OS and architecture.' }
    }
    $env:CGO_ENABLED = '0'
    $env:GOOS = $TargetOS
    $env:GOARCH = $TargetArch
    foreach ($core in @('nekoray_core', 'nekobox_core')) {
        $env:GOTOOLCHAIN = if ($core -eq 'nekoray_core') { 'go1.21.13' } else { 'go1.26.4' }
        Push-Location (Join-Path $projectRoot "go/cmd/$core")
        try {
            $tagArguments = if ($core -eq 'nekobox_core') { @('-tags', $tags) } else { @() }
            if ($Test) {
                Write-Host "Testing $core"
                & go test @tagArguments -count=1 -timeout 90s ./...
                if ($LASTEXITCODE -ne 0) { throw "$core tests failed" }
            }
            $output = Join-Path $destination ($core + $extension)
            Write-Host "Building $core"
            & go build -mod=readonly @tagArguments -trimpath -ldflags "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$version" -o $output .
            if ($LASTEXITCODE -ne 0) { throw "$core build failed" }
            $module = if ($core -eq 'nekoray_core') { 'github.com/xtls/xray-core' } else { 'github.com/sagernet/sing-box' }
            $moduleInfo = (& go list -m -json $module | Out-String) | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0) { throw "Could not read $core version" }
            $manifest.cores[$core] = [ordered]@{
                module = $module
                version = $moduleInfo.Version
                source_module = if ($moduleInfo.Replace) { $moduleInfo.Replace.Path } else { $moduleInfo.Path }
                source_version = if ($moduleInfo.Replace) { $moduleInfo.Replace.Version } else { $moduleInfo.Version }
                go_version = (& go env GOVERSION).Trim()
                sha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
            }
            if ($core -eq 'nekoray_core') {
                $manifest.cores[$core].variant = 'Matsuri compatibility'
                $manifest.cores[$core].source_revision = '01208225ee7e508044cca8eb6776a117bcecd997'
            }
        } finally { Pop-Location }
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $destination 'core-versions.json') -Encoding utf8
    Write-Host "Cores built in $destination"
} finally {
    foreach ($name in $oldEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process')
    }
}
