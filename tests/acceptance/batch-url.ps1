[CmdletBinding()]
param(
    [string] $CoreDirectory = '',
    [string] $OutputDirectory = '',
    [ValidateRange(2, 32)] [int] $Concurrency = 6,
    [ValidateRange(8, 64)] [int] $NodeCount = 24,
    [ValidateRange(80, 1000)] [int] $DelayMs = 200,
    [ValidateRange(1, 5)] [int] $Repeat = 3
)

$ErrorActionPreference = 'Stop'
$batchRepository = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ($Concurrency -gt $NodeCount) {
    throw 'Concurrency must not exceed NodeCount.'
}
if ([string]::IsNullOrWhiteSpace($CoreDirectory)) {
    $CoreDirectory = Join-Path $batchRepository 'build\dynamic-core-check\bin'
}
$batchBinaryDirectory = (Resolve-Path -LiteralPath $CoreDirectory).Path
$batchExtension = if ([System.Environment]::OSVersion.Platform -eq 'Win32NT') { '.exe' } else { '' }
foreach ($batchName in @('nekoray_core', 'nekobox_core')) {
    $batchProgram = Join-Path $batchBinaryDirectory ($batchName + $batchExtension)
    if (-not (Test-Path -LiteralPath $batchProgram -PathType Leaf)) {
        throw "Missing core executable: $batchProgram"
    }
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $batchRepository 'build\dynamic-core-check\batch-acceptance'
}
$batchOutputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$batchRunDirectory = Join-Path $batchOutputRoot ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $batchRunDirectory -Force | Out-Null
$batchGo = (Get-Command go -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$batchEnvironmentNames = @('GOTOOLCHAIN', 'NEKORAY_CORE_BIN_DIR', 'NEKORAY_BATCH_NODES',
    'NEKORAY_BATCH_CONCURRENCY', 'NEKORAY_BATCH_DELAY_MS', 'NEKORAY_BATCH_REPORT')
$batchOriginalEnvironment = @{}
foreach ($batchVariable in $batchEnvironmentNames) {
    $batchOriginalEnvironment[$batchVariable] = [Environment]::GetEnvironmentVariable($batchVariable, 'Process')
}
$batchSummaries = @()
Push-Location -LiteralPath (Join-Path $batchRepository 'go\grpc_server')
try {
    $env:GOTOOLCHAIN = 'go1.21.13'
    $env:NEKORAY_CORE_BIN_DIR = $batchBinaryDirectory
    $env:NEKORAY_BATCH_NODES = [string] $NodeCount
    $env:NEKORAY_BATCH_CONCURRENCY = [string] $Concurrency
    $env:NEKORAY_BATCH_DELAY_MS = [string] $DelayMs
    for ($batchRound = 1; $batchRound -le $Repeat; $batchRound++) {
        $batchReportPath = Join-Path $batchRunDirectory ('round-{0:D2}.json' -f $batchRound)
        $batchLogPath = Join-Path $batchRunDirectory ('round-{0:D2}.log' -f $batchRound)
        $env:NEKORAY_BATCH_REPORT = $batchReportPath
        Write-Host "Round $batchRound/$Repeat`: $NodeCount nodes, total concurrency $Concurrency, fixture delay $DelayMs ms"
        & $batchGo test -mod=readonly -v -count=1 -run '^TestMixedCoreBatchAcceptance$' -timeout 180s ./testutil 2>&1 |
            Tee-Object -FilePath $batchLogPath
        $batchGoExitCode = $LASTEXITCODE
        if ($batchGoExitCode -ne 0 -or -not (Test-Path -LiteralPath $batchReportPath -PathType Leaf)) {
            throw "Batch acceptance failed (exit $batchGoExitCode). Inspect $batchLogPath and $batchReportPath"
        }
        $batchReport = Get-Content -LiteralPath $batchReportPath -Raw | ConvertFrom-Json
        if ($batchReport.status -ne 'passed') {
            throw "Batch acceptance reported failure: $batchReportPath"
        }
        $batchSerial = $batchReport.phases | Where-Object name -EQ 'healthy_serial'
        $batchMixed = $batchReport.phases | Where-Object name -EQ 'healthy_mixed_batch'
        $batchSummaries += [pscustomobject]@{
            round = $batchRound
            compared = $batchReport.compared_results
            matched = $batchReport.matched_results
            serial_ms = $batchSerial.elapsed_ms
            mixed_ms = $batchMixed.elapsed_ms
            speedup = [Math]::Round($batchReport.throughput_speedup, 2)
            actual_peak = $batchMixed.server_peak
            budget = $Concurrency
            cancel_ms = $batchReport.cancel_to_return_ms
            status = $batchReport.status
            report = $batchReportPath
        }
    }
    $batchSummaries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $batchRunDirectory 'summary.json') -Encoding UTF8
    $batchSummaries | Select-Object round, matched, compared, serial_ms, mixed_ms, speedup, actual_peak, budget, cancel_ms, status |
        Format-Table -AutoSize
    Write-Host "Reports: $batchRunDirectory"
} finally {
    foreach ($batchVariable in $batchEnvironmentNames) {
        [Environment]::SetEnvironmentVariable($batchVariable, $batchOriginalEnvironment[$batchVariable], 'Process')
    }
    Pop-Location
}
