# Source-only inventory. Keep this separate so packaging privacy checks can run
# without copying DLLs, creating an archive, or reading runtime snapshots.
function Test-PackageSourcePath {
    param([Parameter(Mandatory)][string]$Relative, [switch]$IsDirectory)
    $relativePath = $Relative.Replace('\', '/')
    if ($relativePath.StartsWith('/') -or $relativePath.Contains(':') -or $relativePath.Contains([char]0)) { return $false }
    $parts = $relativePath.Split('/')
    if (@($parts | Where-Object { $_ -in @('', '.', '..') }).Count) { return $false }
    $directories = @()
    if ($IsDirectory) { $directories = $parts }
    elseif ($parts.Count -gt 1) { $directories = $parts[0..($parts.Count - 2)] }
    if ($directories.Count -and $directories[0] -match '^(build.*|cmake-build-.*|deployment|config|out)$') { return $false }
    if (@($directories | Where-Object { $_ -match '^(build.*|cmake-build-.*|deployment)$' }).Count) { return $false }
    if (@($parts | Where-Object { $_ -in @('.git', '__pycache__', '.venv', 'node_modules') }).Count) { return $false }
    if ($relativePath -match '\.(log|dmp|dpapi|pyc|pyo|pdb|obj|o|exe|dll|ilk|idb|tlog)$') { return $false }
    return $true
}

function Resolve-PackageSourceFile {
    param([Parameter(Mandatory)][string]$ProjectRoot, [Parameter(Mandatory)][string]$Relative)
    if (!(Test-PackageSourcePath $Relative)) { return $null }
    $root = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $path = [IO.Path]::GetFullPath((Join-Path $root $Relative))
    if (!$path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source path escapes the repository.'
    }
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $part = $path
    while ($true) {
        if ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Source links cannot be captured: $Relative"
        }
        if ($part.Equals($root, [StringComparison]::OrdinalIgnoreCase)) { break }
        $part = Split-Path -Parent $part
    }
    return $path
}

function Get-PackageSourceInventory {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [string[]]$Submodules = @('3rdparty/QHotkey', '3rdparty/qjs')
    )
    $root = (Resolve-Path -LiteralPath $ProjectRoot).Path
    $gitFiles = @(& git -C $root -c core.quotepath=false ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate the source tree.' }
    $sourceFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in $gitFiles) {
        if (Test-PackageSourcePath $relative) { [void]$sourceFiles.Add($relative) }
    }
    foreach ($submodule in $Submodules) {
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push((Join-Path $root $submodule))
        while ($pending.Count) {
            $directory = $pending.Pop()
            if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Submodule source directories must not be links.'
            }
            foreach ($file in Get-ChildItem -LiteralPath $directory -Force) {
                $relative = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
                if (!(Test-PackageSourcePath $relative -IsDirectory:$file.PSIsContainer)) { continue }
                # Also prune generated directories inside a submodule before
                # enumerating them; git ignores do not cover extracted sources.
                if ($file.PSIsContainer) {
                    if ($file.Name -match '^(build.*|cmake-build-.*|deployment|config|out)$') { continue }
                    $pending.Push($file.FullName)
                } else {
                    [void]$sourceFiles.Add($relative)
                }
            }
        }
    }
    foreach ($relative in ($sourceFiles | Sort-Object)) {
        if ($null -ne (Resolve-PackageSourceFile -ProjectRoot $root -Relative $relative)) { $relative }
    }
}
