#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'libs/package_source_inventory.ps1')
$cases = [ordered]@{
    'ui/mainwindow.cpp' = $true
    'tests/acceptance/package_gui_probe.cpp' = $true
    'tests/acceptance/package_fixture.py' = $true
    'go/cmd/nekoray_core/config.go' = $true
    'BUILD-SOURCE.json' = $true
    'libs/build-windows.ps1' = $true
    'build/system-acceptance/proxy-snapshot.dpapi' = $false
    'build/system-acceptance/config-hashes.json' = $false
    'build64/system-acceptance/config-hashes.json' = $false
    'build_debug/system-acceptance/state.json' = $false
    'deployment/packages/example/config/profiles/1.json' = $false
    'config/groups/nekoray.json' = $false
    'tests/acceptance/__pycache__/package_fixture.cpython-314.pyc' = $false
    'tests/acceptance/__pycache__/untracked-note.txt' = $false
    'runtime/system.log' = $false
    'runtime/proxy.dpapi' = $false
    '../outside.cpp' = $false
    'C:/outside.cpp' = $false
}
foreach ($relative in $cases.Keys) {
    if ((Test-PackageSourcePath $relative) -ne $cases[$relative]) { throw "Source path policy failed: $relative" }
}
$fixtureRoot = Join-Path $repo ('build/package-source-inventory-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
& git -C $fixtureRoot init --quiet
if ($LASTEXITCODE -ne 0) { throw 'Could not create the synthetic inventory repository.' }
foreach ($relative in @('visible.cpp', 'build/system/proxy.dpapi', 'deployment/private/config.json',
        'build64/system/config-hashes.json', 'build_debug/system/state.json',
        'tests/acceptance/package_fixture.py', 'tests/acceptance/__pycache__/note.txt',
        '3rdparty/fixture/source.cpp', '3rdparty/fixture/build/private.json')) {
    $path = Join-Path $fixtureRoot $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, 'Synthetic inventory fixture; no user data.')
}
# Even a mistakenly tracked runtime file must be excluded.
& git -C $fixtureRoot add -- 'build/system/proxy.dpapi'
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the synthetic runtime-file test.' }
$inventory = @(Get-PackageSourceInventory -ProjectRoot $fixtureRoot -Submodules @('3rdparty/fixture'))
$expected = @('3rdparty/fixture/source.cpp', 'tests/acceptance/package_fixture.py', 'visible.cpp')
$difference = @(Compare-Object ($inventory | Sort-Object) ($expected | Sort-Object))
if ($difference.Count) { throw 'The source inventory included a runtime file or omitted source.' }
Write-Host "Passed $($cases.Count) path-policy checks and synthetic tracked/untracked/submodule inventory checks."
Write-Host "Synthetic fixture retained for inspection: $fixtureRoot"
