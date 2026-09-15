<#
.SYNOPSIS
Runs the opt-in GUI acceptance against fresh local protocol peers.
.DESCRIPTION
Compiles the Go test fixture with the repository's core build tags, starts it
on loopback, and runs only newProtocolPeerRuntimeAcceptance offscreen. Each run
has its own directory and manifest. Logs are retained without printing the
manifest's synthetic credentials. No system proxy or TUN mode is enabled.
.PARAMETER GoPath
Path to the Go executable. The selected local toolchain must satisfy go.mod.
.PARAMETER FixturePath
Optional already-compiled fixture test executable from this source tree and
the same core build tags. It is copied into the new run directory; compilation
is skipped. Omit this option for a source build.
#>
[CmdletBinding()]
param(
    [string] $GuiTests = '',
    [string] $QtBin = '',
    [string] $OutputDirectory = '',
    [string] $GoPath = '',
    [string] $FixturePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne 'Win32NT') {
    throw 'Protocol GUI acceptance requires Windows.'
}

# Start-Process joins ArgumentList into a Windows command line. Quote each
# argument using the CRT rules, including quotes and trailing backslashes.
function ConvertTo-ProtocolGuiArgument([string] $Value) {
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    return '"' + [regex]::Replace($escaped, '(\\+)$', '$1$1') + '"'
}

function Start-ProtocolGuiProcess {
    param([string] $Executable, [string[]] $Arguments, [string] $Directory,
          [string] $StandardOutput, [string] $StandardError)
    $argumentLine = ($Arguments | ForEach-Object { ConvertTo-ProtocolGuiArgument $_ }) -join ' '
    $process = Start-Process -FilePath $Executable -ArgumentList $argumentLine `
        -WorkingDirectory $Directory -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $StandardOutput -RedirectStandardError $StandardError
    # Retain the original kernel handle even if the process exits quickly.
    return [pscustomobject] @{ Process = $process; Handle = $process.SafeHandle }
}

function Stop-ProtocolGuiProcess($OwnedProcess) {
    if ($null -eq $OwnedProcess -or $OwnedProcess.Process.HasExited) { return $false }
    $terminated = [Nekoray.ProtocolGuiAcceptance.NativeProcess]::TerminateProcess($OwnedProcess.Handle, 124)
    if (-not $terminated -and -not $OwnedProcess.Process.HasExited) {
        throw 'Could not terminate the retained acceptance process handle.'
    }
    if (-not $OwnedProcess.Process.WaitForExit(5000)) {
        throw 'The retained acceptance process did not exit after termination.'
    }
    return $true
}

$protocolRepository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($GuiTests)) {
    $GuiTests = Join-Path $protocolRepository 'build\core-upgrade-check\gui-check\nekoray_gui_tests.exe'
}
if ([string]::IsNullOrWhiteSpace($QtBin)) {
    $QtBin = Join-Path $protocolRepository 'build\core-upgrade-check\qt-sdk\Qt6.5.2-Windows-x86_64-VS2022-17.6.5\bin'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $protocolRepository 'build\core-upgrade-check\protocol-gui-acceptance'
}
$protocolOutputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$protocolRunName = 'run-{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'), [Guid]::NewGuid().ToString('N')
$protocolRunDirectory = Join-Path $protocolOutputRoot $protocolRunName
New-Item -ItemType Directory -Path $protocolRunDirectory | Out-Null
$protocolManifest = Join-Path $protocolRunDirectory 'peers.json'
$protocolStop = $protocolManifest + '.stop'
$protocolExecutable = Join-Path $protocolRunDirectory 'protocol-peers.test.exe'
$protocolSummary = Join-Path $protocolRunDirectory 'result.json'
$protocolSlot = 'newProtocolPeerRuntimeAcceptance'
$protocolReport = [ordered] @{
    status = 'failed'
    startedAt = [DateTime]::UtcNow.ToString('o')
    finishedAt = $null
    failedPhase = $null
    runDirectory = $protocolRunDirectory
    build = [ordered] @{
        mode = $(if ([string]::IsNullOrWhiteSpace($FixturePath)) { 'compile' } else { 'prebuilt' })
        go = $null
        tags = $null
        stdout = Join-Path $protocolRunDirectory 'build.stdout.log'
        stderr = Join-Path $protocolRunDirectory 'build.stderr.log'
        exitCode = $null
        isolatedJob = $false
        forcedTermination = $false
    }
    fixture = [ordered] @{
        source = $null
        executable = $protocolExecutable
        sha256 = $null
        manifest = $protocolManifest
        stdout = Join-Path $protocolRunDirectory 'fixture.stdout.log'
        stderr = Join-Path $protocolRunDirectory 'fixture.stderr.log'
        exitCode = $null
        stopRequested = $false
        forcedTermination = $false
    }
    gui = [ordered] @{
        executable = $null
        slot = $protocolSlot
        log = Join-Path $protocolRunDirectory 'gui.qtest.log'
        stdout = Join-Path $protocolRunDirectory 'gui.stdout.log'
        stderr = Join-Path $protocolRunDirectory 'gui.stderr.log'
        exitCode = $null
        slotPassed = $false
        forcedTermination = $false
    }
    environmentRestored = $false
    errors = @()
}
$protocolEnvironmentNames = @('PATH', 'QT_QPA_PLATFORM', 'NEKORAY_TEST_CORE',
                            'NEKORAY_PROTOCOL_GUI_FIXTURE', 'GOTOOLCHAIN')
$protocolOriginalEnvironment = @{}
foreach ($protocolName in $protocolEnvironmentNames) {
    $protocolOriginalEnvironment[$protocolName] = [Environment]::GetEnvironmentVariable($protocolName, 'Process')
}
$protocolBuild = $null
$protocolBuildJob = $null
$protocolFixture = $null
$protocolGui = $null
$protocolErrors = [Collections.Generic.List[string]]::new()
$protocolPhase = 'preflight'

try {
    $protocolGuiPath = (Resolve-Path -LiteralPath $GuiTests).ProviderPath
    if (-not (Test-Path -LiteralPath $protocolGuiPath -PathType Leaf) -or
        [IO.Path]::GetFileName($protocolGuiPath) -ine 'nekoray_gui_tests.exe') {
        throw 'GuiTests must point to nekoray_gui_tests.exe.'
    }
    $protocolGuiDirectory = Split-Path -Parent $protocolGuiPath
    foreach ($protocolCore in @('nekoray_core.exe', 'nekobox_core.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $protocolGuiDirectory $protocolCore) -PathType Leaf)) {
            throw "Missing sibling core executable: $protocolCore"
        }
    }
    $protocolQtDirectory = (Resolve-Path -LiteralPath $QtBin).ProviderPath
    if (-not (Test-Path -LiteralPath $protocolQtDirectory -PathType Container)) {
        throw 'QtBin must point to the Qt DLL directory used to build the GUI tests.'
    }
    if (Test-Path -LiteralPath $protocolManifest) { throw 'The fixture manifest path must be new.' }
    if (Test-Path -LiteralPath $protocolStop) { throw 'The fixture stop path must be new.' }
    $protocolReport.gui.executable = $protocolGuiPath
    $protocolTags = [IO.File]::ReadAllText((Join-Path $protocolRepository 'libs\core_build_tags.txt')).Trim()
    if ([string]::IsNullOrWhiteSpace($protocolTags)) { throw 'Core build tags are empty.' }
    $protocolReport.build.tags = $protocolTags

    if (-not ('Nekoray.ProtocolGuiAcceptance.NativeProcess' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace Nekoray.ProtocolGuiAcceptance {
    public static class NativeProcess {
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool TerminateProcess(SafeProcessHandle process, uint exitCode);

        [StructLayout(LayoutKind.Sequential)]
        private struct BasicLimits {
            public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct IoCounters {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
            public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct ExtendedLimits {
            public BasicLimits Basic;
            public IoCounters Io;
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetInformationJobObject(SafeFileHandle job, int infoClass,
                                                           ref ExtendedLimits limits, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AssignProcessToJobObject(SafeFileHandle job, SafeProcessHandle process);

        public static SafeFileHandle AttachBuildJob(SafeProcessHandle process) {
            var job = CreateJobObject(IntPtr.Zero, null);
            if (job.IsInvalid) {
                int code = Marshal.GetLastWin32Error();
                job.Dispose();
                throw new Win32Exception(code, "Could not create the isolated compiler job.");
            }
            try {
                var limits = new ExtendedLimits();
                limits.Basic.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if (!SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits))))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not set compiler job limits.");
                if (!AssignProcessToJobObject(job, process))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not assign the compiler launcher to its job.");
                return job;
            } catch {
                job.Dispose();
                throw;
            }
        }
    }
}
'@
    }

    $protocolPhase = 'compile'
    if ([string]::IsNullOrWhiteSpace($FixturePath)) {
        if ([string]::IsNullOrWhiteSpace($GoPath)) {
            $GoPath = (Get-Command go.exe -CommandType Application | Select-Object -First 1).Source
        }
        $protocolGoPath = (Resolve-Path -LiteralPath $GoPath).ProviderPath
        if (-not (Test-Path -LiteralPath $protocolGoPath -PathType Leaf)) { throw 'GoPath must point to the Go executable.' }
        $protocolReport.build.go = $protocolGoPath
        [Environment]::SetEnvironmentVariable('GOTOOLCHAIN', 'local', 'Process')
        Write-Host "Compiling protocol fixture. Logs: $protocolRunDirectory"
        # The hidden launcher cannot start Go until it belongs to our job.
        # Assigning an already-running Go process could miss early children.
        $protocolBuildWrapper = Join-Path $protocolRunDirectory 'build-fixture.ps1'
        $protocolBuildGate = Join-Path $protocolRunDirectory 'build.start'
        @'
param([string] $GoExecutable, [string] $BuildTags, [string] $FixtureExecutable, [string] $StartGate)
$ErrorActionPreference = 'Stop'
$buildGateWatch = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path -LiteralPath $StartGate -PathType Leaf)) {
    if ($buildGateWatch.Elapsed.TotalSeconds -ge 30) { throw 'Compiler job was not attached within 30 seconds.' }
    Start-Sleep -Milliseconds 100
}
# Native diagnostic output is retained by the parent's redirected streams.
$ErrorActionPreference = 'Continue'
& $GoExecutable test -c -mod=readonly -tags $BuildTags -o $FixtureExecutable .
if ($null -eq $LASTEXITCODE) { exit 1 }
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $protocolBuildWrapper -Encoding UTF8
        $protocolPowerShell = Join-Path $PSHOME 'powershell.exe'
        if (-not (Test-Path -LiteralPath $protocolPowerShell -PathType Leaf)) {
            $protocolPowerShell = Join-Path $PSHOME 'pwsh.exe'
        }
        $protocolBuild = Start-ProtocolGuiProcess -Executable $protocolPowerShell `
            -Arguments @('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                $protocolBuildWrapper, '-GoExecutable', $protocolGoPath, '-BuildTags', $protocolTags,
                '-FixtureExecutable', $protocolExecutable, '-StartGate', $protocolBuildGate) `
            -Directory (Join-Path $protocolRepository 'go\cmd\nekobox_core') `
            -StandardOutput $protocolReport.build.stdout -StandardError $protocolReport.build.stderr
        $protocolBuildJob = [Nekoray.ProtocolGuiAcceptance.NativeProcess]::AttachBuildJob($protocolBuild.Handle)
        $protocolReport.build.isolatedJob = $true
        [IO.File]::WriteAllText($protocolBuildGate, 'start')
        $protocolBuildWatch = [Diagnostics.Stopwatch]::StartNew()
        while (-not $protocolBuild.Process.WaitForExit(250)) {
            if ($protocolBuildWatch.Elapsed.TotalSeconds -ge 900) { throw 'Fixture compilation exceeded 900 seconds.' }
        }
        $protocolReport.build.exitCode = $protocolBuild.Process.ExitCode
        if ($protocolReport.build.exitCode -ne 0) { throw 'Fixture compilation failed; inspect the build logs.' }
    } else {
        $protocolPhase = 'copy-prebuilt-fixture'
        $protocolFixtureSource = (Resolve-Path -LiteralPath $FixturePath).ProviderPath
        if (-not (Test-Path -LiteralPath $protocolFixtureSource -PathType Leaf) -or
            [IO.Path]::GetExtension($protocolFixtureSource) -ine '.exe') {
            throw 'FixturePath must point to an already-compiled Windows fixture test executable.'
        }
        $protocolReport.fixture.source = $protocolFixtureSource
        Copy-Item -LiteralPath $protocolFixtureSource -Destination $protocolExecutable
    }
    if (-not (Test-Path -LiteralPath $protocolExecutable -PathType Leaf)) { throw 'The fixture executable was not produced.' }
    $protocolReport.fixture.sha256 = (Get-FileHash -LiteralPath $protocolExecutable -Algorithm SHA256).Hash

    $protocolPhase = 'fixture-readiness'
    [Environment]::SetEnvironmentVariable('QT_QPA_PLATFORM', 'offscreen', 'Process')
    [Environment]::SetEnvironmentVariable('NEKORAY_TEST_CORE', 'ray', 'Process')
    [Environment]::SetEnvironmentVariable('NEKORAY_PROTOCOL_GUI_FIXTURE', $protocolManifest, 'Process')
    [Environment]::SetEnvironmentVariable('PATH', $protocolQtDirectory + ';' + $protocolOriginalEnvironment['PATH'], 'Process')
    $protocolFixture = Start-ProtocolGuiProcess -Executable $protocolExecutable `
        -Arguments @('-test.run', '^TestProtocolGuiPeers$', '-test.timeout', '210s', '-test.v') `
        -Directory $protocolRunDirectory -StandardOutput $protocolReport.fixture.stdout -StandardError $protocolReport.fixture.stderr
    $protocolReadyWatch = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $protocolManifest -PathType Leaf)) {
        if ($protocolFixture.Process.HasExited) { throw 'Fixture exited before publishing its manifest; inspect the fixture logs.' }
        if ($protocolReadyWatch.Elapsed.TotalSeconds -ge 45) { throw 'Fixture did not publish its manifest within 45 seconds.' }
        Start-Sleep -Milliseconds 100
    }
    if ($protocolFixture.Process.HasExited) { throw 'Fixture exited before GUI acceptance could start.' }

    $protocolPhase = 'gui'
    Write-Host "Running $protocolSlot with fresh loopback peers."
    $protocolGui = Start-ProtocolGuiProcess -Executable $protocolGuiPath `
        -Arguments @($protocolSlot, '-v1', '-o', ($protocolReport.gui.log + ',txt')) `
        -Directory $protocolRunDirectory -StandardOutput $protocolReport.gui.stdout -StandardError $protocolReport.gui.stderr
    $protocolGuiWatch = [Diagnostics.Stopwatch]::StartNew()
    while (-not $protocolGui.Process.WaitForExit(250)) {
        if ($protocolFixture.Process.HasExited) { throw 'Fixture exited while GUI acceptance was still running.' }
        if ($protocolGuiWatch.Elapsed.TotalSeconds -ge 150) { throw 'GUI acceptance exceeded 150 seconds.' }
    }
    $protocolReport.gui.exitCode = $protocolGui.Process.ExitCode
    if ($protocolReport.gui.exitCode -ne 0) { throw 'GUI acceptance failed; inspect the GUI logs.' }
    if (-not (Test-Path -LiteralPath $protocolReport.gui.log -PathType Leaf)) { throw 'GUI acceptance did not write its QTest log.' }
    $protocolQTestLog = [IO.File]::ReadAllText($protocolReport.gui.log)
    $protocolReport.gui.slotPassed = [regex]::IsMatch($protocolQTestLog,
        '(?m)^PASS\s+:\s+GuiRegressionTest::newProtocolPeerRuntimeAcceptance\(\)\s*$')
    if (-not $protocolReport.gui.slotPassed) { throw 'The selected GUI slot did not report PASS; a skipped test is not acceptance.' }
} catch {
    $protocolReport.failedPhase = $protocolPhase
    $protocolErrors.Add($_.Exception.Message)
} finally {
    # Stop only the exact processes launched above, never processes by name or
    # a reopened PID. The fixture also has its own finite lifetime as a guard.
    if ($null -ne $protocolBuildJob) {
        try {
            $protocolBuildWasRunning = -not $protocolBuild.Process.HasExited
            $protocolBuildJob.Dispose()
            if ($protocolBuildWasRunning) {
                $protocolReport.build.forcedTermination = $true
                $protocolErrors.Add('The owned compiler job required forced termination.')
                if (-not $protocolBuild.Process.WaitForExit(5000)) {
                    throw 'The compiler launcher did not exit when its job closed.'
                }
            }
        } catch {
            $protocolErrors.Add('Cleanup failed for the owned compiler job.')
        }
    }
    foreach ($protocolOwned in @(
        @{ Instance = $protocolGui; Report = $protocolReport.gui; Name = 'GUI' },
        @{ Instance = $protocolBuild; Report = $protocolReport.build; Name = 'compiler' }
    )) {
        if ($null -eq $protocolOwned.Instance) { continue }
        try {
            if (Stop-ProtocolGuiProcess $protocolOwned.Instance) {
                $protocolOwned.Report.forcedTermination = $true
                $protocolErrors.Add("The owned $($protocolOwned.Name) process required forced termination.")
            }
            $protocolOwned.Report.exitCode = $protocolOwned.Instance.Process.ExitCode
        } catch {
            $protocolErrors.Add("Cleanup failed for the owned $($protocolOwned.Name) process.")
        }
    }
    if ($null -ne $protocolFixture) {
        try {
            if (-not (Test-Path -LiteralPath $protocolStop)) {
                $protocolStopFile = [IO.File]::Open($protocolStop, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                $protocolStopFile.Dispose()
            }
            $protocolReport.fixture.stopRequested = $true
        } catch {
            $protocolErrors.Add('Could not create the fixture stop signal.')
        }
        try {
            if (-not $protocolFixture.Process.WaitForExit(10000)) {
                if (Stop-ProtocolGuiProcess $protocolFixture) {
                    $protocolReport.fixture.forcedTermination = $true
                    $protocolErrors.Add('The owned fixture required forced termination.')
                }
            }
            $protocolReport.fixture.exitCode = $protocolFixture.Process.ExitCode
            if ($protocolReport.fixture.exitCode -ne 0) { $protocolErrors.Add('Fixture did not exit successfully; inspect its logs.') }
        } catch {
            $protocolErrors.Add('Cleanup failed for the owned fixture process.')
        }
    }
    $protocolReport.environmentRestored = $true
    foreach ($protocolName in $protocolEnvironmentNames) {
        try {
            [Environment]::SetEnvironmentVariable($protocolName, $protocolOriginalEnvironment[$protocolName], 'Process')
        } catch {
            $protocolReport.environmentRestored = $false
            $protocolErrors.Add("Could not restore process environment variable $protocolName.")
        }
    }
    foreach ($protocolOwned in @($protocolGui, $protocolFixture, $protocolBuild)) {
        if ($null -ne $protocolOwned) { $protocolOwned.Process.Dispose() }
    }
    if ($protocolErrors.Count -eq 0 -and $protocolReport.gui.slotPassed -and
        $protocolReport.fixture.stopRequested -and $protocolReport.fixture.exitCode -eq 0) {
        $protocolReport.status = 'passed'
    } elseif ($null -eq $protocolReport.failedPhase) {
        $protocolReport.failedPhase = 'cleanup'
    }
    $protocolReport.errors = $protocolErrors.ToArray()
    $protocolReport.finishedAt = [DateTime]::UtcNow.ToString('o')
    $protocolReport | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $protocolSummary -Encoding UTF8
}

if ($protocolReport.status -ne 'passed') {
    throw "Protocol GUI acceptance FAILED ($($protocolReport.failedPhase)). Inspect $protocolSummary"
}
Write-Host "Protocol GUI acceptance PASSED. Report: $protocolSummary"
