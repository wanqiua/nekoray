param(
    [Parameter(Mandatory)][int]$TargetPid,
    [Parameter(Mandatory)][long]$Created,
    [Parameter(Mandatory)][long]$Window,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Report
)

$ErrorActionPreference = 'Stop'
$result = @{ identity_verified = $false; groups_visible = @(); exit_invoked = $false }
function Save-State([string]$Phase) {
    $result.phase = $Phase
    [System.IO.File]::WriteAllText($Report, ($result | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))
}
function Assert-TargetIdentity {
    $targetProcess = [System.Diagnostics.Process]::GetProcessById($TargetPid)
    try {
        if ($targetProcess.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $Created -or
            -not [string]::Equals($targetProcess.MainModule.FileName, $Executable, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The owned GUI identity changed.'
        }
    } finally { $targetProcess.Dispose() }
}

try {
    Assert-TargetIdentity
    $result.identity_verified = $true
    Save-State 'identity-verified'
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    Add-Type -AssemblyName WindowsBase
    # Qt's tool button enters QMenu::exec during a synchronous accessibility
    # invocation. Post a normal button click to this verified window only;
    # neither global mouse input nor another application's window is touched.
    Add-Type -ReferencedAssemblies @(
        [System.Windows.Automation.AutomationElement].Assembly.Location,
        [System.Windows.Automation.ControlType].Assembly.Location,
        [System.Windows.Rect].Assembly.Location
    ) -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows.Automation;
public static class OwnedGuiMenuActions {
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rectangle { public int Left; public int Top; public int Right; public int Bottom; }
    private delegate bool EnumCallback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll")] private static extern bool ScreenToClient(IntPtr handle, ref Point point);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr handle, out Rectangle rectangle);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr handle, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr handle);
    public static void Open(AutomationElement menu, IntPtr window, int pid, long created, string executable) {
        using (Process process = Process.GetProcessById(pid)) {
            if (process.StartTime.ToUniversalTime().ToFileTimeUtc() != created ||
                !String.Equals(process.MainModule.FileName, executable, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The owned GUI identity changed.");
        }
        uint owner;
        GetWindowThreadProcessId(window, out owner);
        if (owner != pid || menu.Current.ProcessId != pid || menu.Current.IsOffscreen)
            throw new InvalidOperationException("The menu no longer belongs to the visible owned GUI.");
        var bounds = menu.Current.BoundingRectangle;
        if (bounds.IsEmpty || bounds.Width <= 0 || bounds.Height <= 0)
            throw new InvalidOperationException("The owned menu has no visible button bounds.");
        Point point = new Point { X = (int)(bounds.Left + bounds.Width / 2), Y = (int)(bounds.Top + bounds.Height / 2) };
        Rectangle client;
        if (!ScreenToClient(window, ref point) || !GetClientRect(window, out client) ||
            point.X < 0 || point.Y < 0 || point.X >= client.Right || point.Y >= client.Bottom)
            throw new InvalidOperationException("The menu button is outside its owned client window.");
        IntPtr position = new IntPtr((point.Y << 16) | (point.X & 65535));
        if (!PostMessage(window, 0x0200, IntPtr.Zero, position) ||
            !PostMessage(window, 0x0201, new IntPtr(1), position) ||
            !PostMessage(window, 0x0202, IntPtr.Zero, position))
            throw new InvalidOperationException("The owned menu click could not be posted.");
    }
    public static IntPtr[] Windows(int pid) {
        List<IntPtr> handles = new List<IntPtr>();
        EnumCallback visit = delegate(IntPtr handle, IntPtr parameter) {
            uint owner;
            GetWindowThreadProcessId(handle, out owner);
            if (owner == pid && IsWindowVisible(handle)) handles.Add(handle);
            return true;
        };
        if (!EnumWindows(visit, IntPtr.Zero)) throw new InvalidOperationException("Owned window enumeration failed.");
        return handles.ToArray();
    }
}
'@
    Save-State 'automation-loaded'
    $rootElement = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$Window)
    if ($null -eq $rootElement -or $rootElement.Current.ProcessId -ne $TargetPid) {
        throw 'The supplied main window no longer belongs to the owned GUI.'
    }
    Save-State 'window-verified'
    $all = $rootElement.FindAll([System.Windows.Automation.TreeScope]::Subtree, [System.Windows.Automation.Condition]::TrueCondition)
    Save-State 'controls-enumerated'
    $groups = @('合成订阅组', '手动测试组')
    $seen = @()
    foreach ($element in $all) {
        if ($element.Current.ProcessId -eq $TargetPid) {
            foreach ($group in $groups) {
                if ($element.Current.Name.StartsWith($group)) { $seen += $group }
            }
        }
    }
    $result.groups_visible = @($seen | Select-Object -Unique)
    Save-State 'groups-inspected'
    $programMenu = $null
    $programTypes = @([System.Windows.Automation.ControlType]::MenuItem,
                      [System.Windows.Automation.ControlType]::Button,
                      [System.Windows.Automation.ControlType]::SplitButton)
    foreach ($element in $all) {
        if ($element.Current.ProcessId -eq $TargetPid -and
            -not $element.Current.IsOffscreen -and
            $programTypes -contains $element.Current.ControlType -and
            @('Program', '程序') -contains $element.Current.Name.Replace('&', '')) {
            $programMenu = $element
            break
        }
    }
    if ($null -ne $programMenu) {
        Assert-TargetIdentity
        Save-State 'program-menu-opening'
        [OwnedGuiMenuActions]::Open($programMenu, [IntPtr]$Window, $TargetPid, $Created, $Executable)
    }
    Save-State 'program-menu-opened'
    # A Qt popup menu can be a separate top-level window. Restrict every
    # enumerated window and action to this PID and re-check creation identity.
    $menuDeadline = [DateTime]::UtcNow.AddSeconds(4)
    do {
        $ownedHandles = [OwnedGuiMenuActions]::Windows($TargetPid)
        foreach ($ownedHandle in $ownedHandles) {
            $ownedWindow = [System.Windows.Automation.AutomationElement]::FromHandle($ownedHandle)
            if ($ownedWindow.Current.ProcessId -ne $TargetPid) { continue }
            $elements = $ownedWindow.FindAll([System.Windows.Automation.TreeScope]::Subtree, [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($element in $elements) {
                if ($element.Current.ProcessId -eq $TargetPid -and
                    -not $element.Current.IsOffscreen -and
                    $element.Current.ControlType -eq [System.Windows.Automation.ControlType]::MenuItem -and
                    @('Exit', '退出') -contains $element.Current.Name.Replace('&', '')) {
                    $invoke = $null
                    if ($element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invoke)) {
                        Assert-TargetIdentity
                        Save-State 'exit-menu-invoking'
                        $invoke.Invoke()
                        $result.exit_invoked = $true
                        break
                    }
                }
            }
            if ($result.exit_invoked) { break }
        }
        if (-not $result.exit_invoked) { Start-Sleep -Milliseconds 50 }
    } while (-not $result.exit_invoked -and [DateTime]::UtcNow -lt $menuDeadline)
    if (-not $result.exit_invoked) { $result.exit_reason = 'The owned GUI did not expose an invokable Exit menu item.' }
} catch {
    $result.exit_reason = 'The owned GUI accessibility action could not be completed.'
    $result.exit_error_type = $_.Exception.GetType().FullName
    $result.exit_detail = $_.Exception.Message
} finally {
    Save-State 'completed'
}
