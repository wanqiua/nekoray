"""Windows system-mode acceptance with scoped suspension and independent recovery.

Without --execute this only reads the host and writes a redacted preflight report.
The guarded worker owns all suspended-process handles and runs recovery even when
the outer runner disappears. No original installation is restarted or rewritten.
"""

from __future__ import annotations

import argparse
import base64
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

if os.name == "nt":
    import winreg


class AcceptanceError(RuntimeError):
    """Messages are safe to include in the public report."""


GUI_NAMES = {"nekoray.exe", "nekobox.exe"}
CORE_NAMES = {"nekoray_core.exe", "nekobox_core.exe"}
INTERNET_SETTINGS = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings"
PROXY_VALUES = ("ProxyEnable", "ProxyServer", "ProxyOverride", "AutoConfigURL", "AutoDetect")
QUERY = 0x1000
SYNCHRONIZE = 0x100000
SUSPEND_RESUME = 0x0800
TERMINATE = 0x0001
WAIT_TIMEOUT = 258
CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200


def canonical(path) -> str:
    return os.path.normcase(os.path.realpath(path)).removeprefix("\\\\?\\")


def inside(path, root) -> bool:
    try:
        return os.path.commonpath([canonical(path), canonical(root)]) == canonical(root)
    except ValueError:
        return False


def digest(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":")).encode("utf-8")).hexdigest()


def write_json(path: Path, value):
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


class Native:
    def __init__(self):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.nt = ctypes.WinDLL("ntdll", use_last_error=True)
        self.inet = ctypes.WinDLL("wininet", use_last_error=True)
        self.ras = ctypes.WinDLL("rasapi32", use_last_error=True)
        self.advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        self.crypt = ctypes.WinDLL("crypt32", use_last_error=True)
        self.dns = ctypes.WinDLL("dnsapi", use_last_error=True)

        class ProcessEntry(ctypes.Structure):
            _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("pid", w.DWORD),
                        ("heap", ctypes.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                        ("ppid", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD),
                        ("name", w.WCHAR * 260)]

        class OptionValue(ctypes.Union):
            _fields_ = [("number", w.DWORD), ("text", ctypes.c_void_p), ("time", w.FILETIME)]

        class Option(ctypes.Structure):
            _fields_ = [("option", w.DWORD), ("value", OptionValue)]

        class OptionList(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("connection", w.LPWSTR), ("count", w.DWORD),
                        ("error", w.DWORD), ("options", ctypes.POINTER(Option))]

        class RasEntry(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("name", w.WCHAR * 257), ("flags", w.DWORD),
                        ("phonebook", w.WCHAR * 261)]

        class Blob(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]

        self.ProcessEntry, self.Option, self.OptionList = ProcessEntry, Option, OptionList
        self.RasEntry, self.Blob = RasEntry, Blob
        signatures = [
            (self.kernel, "OpenProcess", [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            (self.kernel, "CloseHandle", [w.HANDLE], w.BOOL),
            (self.kernel, "GetCurrentProcess", [], w.HANDLE),
            (self.kernel, "QueryFullProcessImageNameW", [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)], w.BOOL),
            (self.kernel, "GetProcessTimes", [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4, w.BOOL),
            (self.kernel, "WaitForSingleObject", [w.HANDLE, w.DWORD], w.DWORD),
            (self.kernel, "CreateToolhelp32Snapshot", [w.DWORD, w.DWORD], w.HANDLE),
            (self.kernel, "Process32FirstW", [w.HANDLE, ctypes.POINTER(ProcessEntry)], w.BOOL),
            (self.kernel, "Process32NextW", [w.HANDLE, ctypes.POINTER(ProcessEntry)], w.BOOL),
            (self.kernel, "LocalFree", [ctypes.c_void_p], ctypes.c_void_p),
            (self.kernel, "GlobalFree", [ctypes.c_void_p], ctypes.c_void_p),
            (self.nt, "NtSuspendProcess", [w.HANDLE], w.LONG),
            (self.nt, "NtResumeProcess", [w.HANDLE], w.LONG),
            (self.inet, "InternetQueryOptionW", [w.HANDLE, w.DWORD, ctypes.c_void_p, ctypes.POINTER(w.DWORD)], w.BOOL),
            (self.inet, "InternetSetOptionW", [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD], w.BOOL),
            (self.ras, "RasEnumEntriesW", [w.LPCWSTR, w.LPCWSTR, ctypes.POINTER(RasEntry),
                                           ctypes.POINTER(w.DWORD), ctypes.POINTER(w.DWORD)], w.DWORD),
            (self.advapi, "OpenProcessToken", [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)], w.BOOL),
            (self.advapi, "GetTokenInformation", [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD,
                                                  ctypes.POINTER(w.DWORD)], w.BOOL),
            (self.advapi, "ConvertSidToStringSidW", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)], w.BOOL),
            (self.advapi, "ConvertStringSecurityDescriptorToSecurityDescriptorW",
             [w.LPCWSTR, w.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(w.DWORD)], w.BOOL),
            (self.advapi, "SetFileSecurityW", [w.LPCWSTR, w.DWORD, ctypes.c_void_p], w.BOOL),
            (self.crypt, "CryptProtectData", [ctypes.POINTER(Blob), w.LPCWSTR, ctypes.c_void_p,
                                              ctypes.c_void_p, ctypes.c_void_p, w.DWORD, ctypes.POINTER(Blob)], w.BOOL),
            (self.dns, "DnsFlushResolverCache", [], w.BOOL),
        ]
        for library, name, arguments, result in signatures:
            function = getattr(library, name)
            function.argtypes, function.restype = arguments, result

    @staticmethod
    def require(ok, operation):
        if not ok:
            raise AcceptanceError(f"{operation}: Windows error {ctypes.get_last_error()}")

    def process_table(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise AcceptanceError("Cannot enumerate the process tree")
        entries = {}
        try:
            entry = self.ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            available = self.kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while available:
                entries[entry.pid] = {"pid": entry.pid, "ppid": entry.ppid, "name": entry.name}
                available = self.kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            self.kernel.CloseHandle(snapshot)
        return entries

    def identity(self, handle, pid):
        text = ctypes.create_unicode_buffer(32768)
        length = w.DWORD(len(text))
        self.require(self.kernel.QueryFullProcessImageNameW(handle, 0, text, ctypes.byref(length)), "Read process image")
        created, exited, kernel, user = (w.FILETIME() for _ in range(4))
        self.require(self.kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                                ctypes.byref(kernel), ctypes.byref(user)), "Read process creation time")
        return {"pid": pid, "created": (created.dwHighDateTime << 32) | created.dwLowDateTime,
                "image": canonical(text.value)}

    def private_directory(self, path):
        token = w.HANDLE()
        sid_text = ctypes.c_void_p()
        descriptor = ctypes.c_void_p()
        self.require(self.advapi.OpenProcessToken(self.kernel.GetCurrentProcess(), 8, ctypes.byref(token)), "Open current user token")
        try:
            size = w.DWORD()
            self.advapi.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
            buffer = ctypes.create_string_buffer(size.value)
            self.require(self.advapi.GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)), "Read current user identity")
            sid = ctypes.c_void_p.from_buffer(buffer).value
            self.require(self.advapi.ConvertSidToStringSidW(sid, ctypes.byref(sid_text)), "Convert current user identity")
            acl = f"D:P(A;OICI;FA;;;{ctypes.wstring_at(sid_text)})(A;OICI;FA;;;SY)"
            self.require(self.advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW(acl, 1, ctypes.byref(descriptor), None),
                         "Build private report permissions")
            self.require(self.advapi.SetFileSecurityW(str(path), 0x80000004, descriptor), "Protect report directory")
        finally:
            self.kernel.CloseHandle(token)
            if sid_text.value:
                self.kernel.LocalFree(sid_text)
            if descriptor.value:
                self.kernel.LocalFree(descriptor)

    def protect(self, data: bytes) -> bytes:
        buffer = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        source, target = self.Blob(len(data), buffer), self.Blob()
        self.require(self.crypt.CryptProtectData(ctypes.byref(source), "Nekoray acceptance recovery", None, None, None,
                                                 1, ctypes.byref(target)), "Encrypt proxy backup")
        try:
            return ctypes.string_at(target.data, target.size)
        finally:
            self.kernel.LocalFree(target.data)

    def connections(self):
        first = self.RasEntry()
        first.size = ctypes.sizeof(first)
        size, count = w.DWORD(ctypes.sizeof(first)), w.DWORD()
        code = self.ras.RasEnumEntriesW(None, None, ctypes.byref(first), ctypes.byref(size), ctypes.byref(count))
        if code == 603:  # ERROR_BUFFER_TOO_SMALL
            entries = (self.RasEntry * ((size.value + ctypes.sizeof(first) - 1) // ctypes.sizeof(first)))()
            entries[0].size = ctypes.sizeof(first)
            code = self.ras.RasEnumEntriesW(None, None, entries, ctypes.byref(size), ctypes.byref(count))
        else:
            entries = [first]
        if code:
            raise AcceptanceError(f"Cannot enumerate proxy connections: Windows error {code}")
        return [""] + [entries[index].name for index in range(count.value)]

    def proxy_read(self, connection):
        options = (self.Option * 4)()
        for index, option in enumerate(options):
            option.option = index + 1
        listing = self.OptionList(ctypes.sizeof(self.OptionList), connection or None, 4, 0, options)
        size = w.DWORD(ctypes.sizeof(listing))
        try:
            self.require(self.inet.InternetQueryOptionW(None, 75, ctypes.byref(listing), ctypes.byref(size)), "Read system proxy")
            return {"flags": options[0].value.number,
                    **{name: ctypes.wstring_at(options[index].value.text) if options[index].value.text else ""
                       for index, name in enumerate(("", "server", "bypass", "pac")) if index}}
        finally:
            for index in range(1, 4):
                if options[index].value.text:
                    self.kernel.GlobalFree(options[index].value.text)

    def proxy_write(self, connection, state):
        options = (self.Option * 4)()
        strings = [ctypes.create_unicode_buffer(state[key]) for key in ("server", "bypass", "pac")]
        for index, option in enumerate(options):
            option.option = index + 1
            if index:
                option.value.text = ctypes.cast(strings[index - 1], ctypes.c_void_p).value
            else:
                option.value.number = state["flags"]
        listing = self.OptionList(ctypes.sizeof(self.OptionList), connection or None, 4, 0, options)
        self.require(self.inet.InternetSetOptionW(None, 75, ctypes.byref(listing), ctypes.sizeof(listing)), "Restore owned proxy settings")

    def refresh_proxy(self):
        self.require(self.inet.InternetSetOptionW(None, 39, None, 0), "Notify system proxy restoration")
        self.require(self.inet.InternetSetOptionW(None, 37, None, 0), "Refresh system proxy cache")

    def flush_dns_cache(self):
        if not self.dns.DnsFlushResolverCache():
            raise AcceptanceError("Windows DNS resolver cache cleanup returned failure")


class Process:
    def __init__(self, native, pid, rights=QUERY | SYNCHRONIZE, expected=None):
        self.native = native
        self.handle = native.kernel.OpenProcess(rights, False, pid)
        native.require(self.handle, "Open scoped process handle")
        try:
            self.identity = native.identity(self.handle, pid)
            if expected is not None and self.identity != expected:
                raise AcceptanceError("Process identity changed during acceptance preparation")
        except BaseException:
            self.close()
            raise
        self.suspended = False

    def alive(self):
        result = self.native.kernel.WaitForSingleObject(self.handle, 0)
        if result not in (0, WAIT_TIMEOUT):
            raise AcceptanceError("Cannot verify a retained process handle")
        return result == WAIT_TIMEOUT

    def suspend(self):
        if not self.alive():
            raise AcceptanceError("An original process exited before it could be paused")
        status = self.native.nt.NtSuspendProcess(self.handle)
        if status < 0:
            raise AcceptanceError(f"Cannot pause an owned original process: NTSTATUS {status & 0xffffffff:08x}")
        self.suspended = True

    def resume(self):
        if self.suspended:
            status = self.native.nt.NtResumeProcess(self.handle)
            if status < 0 and self.alive():
                raise AcceptanceError(f"Cannot resume an owned original process: NTSTATUS {status & 0xffffffff:08x}")
            self.suspended = False

    def close(self):
        if self.handle:
            self.native.kernel.CloseHandle(self.handle)
            self.handle = None


def installation_processes(native, installation):
    table = native.process_table()
    opened, roots = {}, []
    try:
        for pid, row in table.items():
            if row["name"].lower() not in GUI_NAMES:
                continue
            candidate = Process(native, pid)
            if canonical(Path(candidate.identity["image"]).parent) == canonical(installation):
                expected = candidate.identity
                candidate.close()
                candidate = Process(native, pid, QUERY | SYNCHRONIZE | SUSPEND_RESUME, expected)
                opened[pid] = candidate
                roots.append(pid)
            else:
                candidate.close()
        if not roots:
            raise AcceptanceError("No running GUI was found in the explicitly selected installation")
        order = list(roots)
        for parent in order:
            for pid, row in table.items():
                if row["ppid"] != parent or pid in opened:
                    continue
                child = Process(native, pid)
                # A console host belongs to Windows and has no proxy session
                # to freeze. Exempt only the verified system executable path.
                if child.identity["image"] == canonical(Path(os.environ["SystemRoot"]) / "System32/conhost.exe"):
                    child.close()
                    continue
                if (not inside(child.identity["image"], installation) or
                        child.identity["created"] < opened[parent].identity["created"]):
                    child.close()
                    raise AcceptanceError("A GUI child is outside the selected installation or has a reused parent PID")
                expected = child.identity
                child.close()
                child = Process(native, pid, QUERY | SYNCHRONIZE | SUSPEND_RESUME, expected)
                opened[pid] = child
                order.append(pid)
        # The order starts with all GUIs, then parents before their descendants.
        return [opened[pid] for pid in order], len(roots)
    except BaseException:
        for process in opened.values():
            process.close()
        raise


def registry_proxy():
    result = {}
    for relative, selected in (("", PROXY_VALUES), ("Connections", None)):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS + ("\\" + relative if relative else "")) as key:
                if selected is None:
                    names = []
                    for index in range(winreg.QueryInfoKey(key)[1]):
                        names.append(winreg.EnumValue(key, index)[0])
                else:
                    names = selected
                for name in names:
                    try:
                        value, kind = winreg.QueryValueEx(key, name)
                    except FileNotFoundError:
                        continue
                    result[relative + "/" + name] = {"type": kind,
                        "value": base64.b64encode(value).decode("ascii") if isinstance(value, bytes) else value}
        except FileNotFoundError:
            pass
    return result


def network_snapshot():
    script = r"""
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$result = @{
    routes = @(Get-NetRoute -PolicyStore ActiveStore | Select-Object InterfaceIndex,AddressFamily,DestinationPrefix,NextHop,RouteMetric,Protocol)
    dns = @(Get-DnsClientServerAddress | Select-Object InterfaceIndex,AddressFamily,ServerAddresses)
    adapters = @(Get-NetAdapter -IncludeHidden | Select-Object InterfaceGuid,ifIndex,Name,InterfaceDescription,Status,MacAddress)
}
$result | ConvertTo-Json -Depth 6 -Compress
"""
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                            capture_output=True, timeout=25, creationflags=CREATE_NO_WINDOW)
    if result.returncode:
        raise AcceptanceError(f"Cannot capture network state: PowerShell exit {result.returncode}")
    try:
        value = json.loads(result.stdout.decode("utf-8-sig"))
        return {key: sorted((digest(row) for row in value[key])) for key in ("routes", "dns", "adapters")}
    except (ValueError, KeyError) as error:
        raise AcceptanceError("Network snapshot was incomplete") from error


def config_hashes(installation):
    root = installation / "config"
    if not root.is_dir():
        raise AcceptanceError("The selected installation has no portable config directory to verify")
    hashes = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            path = Path(directory) / name
            if path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
                raise AcceptanceError("Configuration links are not accepted for this host acceptance")
        for name in files:
            path = Path(directory) / name
            with path.open("rb") as stream:
                hashes[path.relative_to(root).as_posix()] = hashlib.file_digest(stream, "sha256").hexdigest()
    return hashes


def proxy_snapshot(native):
    return {"connections": {name: native.proxy_read(name) for name in native.connections()},
            "registry": registry_proxy()}


def safe_proxy_servers(path):
    if not path.is_file():
        return set()
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    servers = value.get("servers", [value.get("server", "")])
    if not isinstance(servers, list) or any(not isinstance(server, str) for server in servers):
        raise AcceptanceError("The synthetic proxy audit has an invalid format")
    allowed = set()
    for server in servers:
        if not server or len(server) > 1024:
            raise AcceptanceError("The synthetic proxy audit contains an invalid address")
        for address in server.split(";"):
            endpoint = address.rsplit("=", 1)[-1]
            host, separator, port = endpoint.rpartition(":")
            if not separator or host.lower() not in ("127.0.0.1", "localhost", "[::1]") or not port.isdigit() or not 0 < int(port) < 65536:
                raise AcceptanceError("The synthetic proxy audit contains a non-loopback address")
        allowed.add(server)
    return allowed


def normalize_registry(snapshot):
    normalized = json.loads(json.dumps(snapshot))
    for name, entry in normalized.items():
        # WinINet increments this DWORD on a settings write, including a normal
        # application restore. It is not a proxy preference or network route.
        if name.startswith("Connections/") and entry["type"] == winreg.REG_BINARY:
            data = bytearray(base64.b64decode(entry["value"]))
            if len(data) >= 12 and int.from_bytes(data[:4], "little") in (0x46, 0x3c):
                data[4:8] = bytes(4)
                entry["value"] = base64.b64encode(data).decode("ascii")
    # WinINet may materialize an absent optional value as its empty/default
    # representation while restoring otherwise identical proxy preferences.
    for name in ("ProxyServer", "ProxyOverride", "AutoConfigURL"):
        entry = normalized.get("/" + name)
        if entry and entry["value"] == "":
            normalized.pop("/" + name)
    return normalized


def restore_proxy(native, before, audit):
    restored = 0
    allowed = safe_proxy_servers(audit)
    for connection, saved in before["connections"].items():
        current = native.proxy_read(connection)
        if current == saved:
            continue
        if (current["server"] in allowed and current["flags"] == 3 and
                current["bypass"] == saved["bypass"] and current["pac"] == saved["pac"]):
            native.proxy_write(connection, saved)
            restored += 1
    native.refresh_proxy()
    after = proxy_snapshot(native)
    return {"connections_restored_by_guardian": restored,
            "semantic_state_restored": before["connections"] == after["connections"],
            "registry_restored": normalize_registry(before["registry"]) == normalize_registry(after["registry"]),
            "raw_registry_changed": before["registry"] != after["registry"],
            "before_digest": digest(before), "after_digest": digest(after)}


def summarize(before_proxy, network, hashes, processes, roots):
    return {"original_gui_count": roots, "original_descendant_count": len(processes) - roots,
            "process_identities_digest": digest([process.identity for process in processes]),
            "config_file_count": len(hashes), "config_digest": digest(hashes),
            "proxy_connection_count": len(before_proxy["connections"]), "proxy_digest": digest(before_proxy),
            "network": {name: {"count": len(rows), "digest": digest(rows)} for name, rows in network.items()}}


def validate_paths(gui_tests, installation, output):
    if not gui_tests.is_file() or gui_tests.name.lower() != "nekoray_gui_tests.exe":
        raise AcceptanceError("--gui-tests must select the built GUI regression test executable")
    if not installation.is_dir() or inside(gui_tests, installation):
        raise AcceptanceError("The test executable must be outside the selected original installation")
    if inside(output, installation):
        raise AcceptanceError("Acceptance output must be outside the original installation")
    if not all((gui_tests.parent / name).is_file() for name in CORE_NAMES):
        raise AcceptanceError("Both test core executables must be beside the GUI test executable")


def inspect_host(native, installation):
    processes, roots = installation_processes(native, installation)
    try:
        proxy, network, hashes = proxy_snapshot(native), network_snapshot(), config_hashes(installation)
        return summarize(proxy, network, hashes, processes, roots)
    finally:
        for process in processes:
            process.close()


def test_core_identities(native, directory):
    identities = {}
    for pid, row in native.process_table().items():
        if row["name"].lower() not in CORE_NAMES:
            continue
        try:
            process = Process(native, pid)
        except AcceptanceError:
            # An inaccessible elevated process is not evidence that it exited.
            # Only ignore a process which actually disappeared during the read.
            current = native.process_table().get(pid)
            if current and current["name"].lower() in CORE_NAMES:
                raise AcceptanceError("Cannot verify whether a core process remains; cleanup is not confirmed")
            continue
        try:
            if canonical(Path(process.identity["image"]).parent) == canonical(directory):
                identities[(pid, process.identity["created"])] = process.identity
        finally:
            process.close()
    return identities


def completed_test_cases(path):
    expected = {"systemProxyRuntimeAcceptance", "tunSystemRuntimeAcceptance"}
    try:
        tree = ET.parse(path)
        completed = set()
        for case in tree.iter("testcase"):
            name = case.get("name", "").rsplit("::", 1)[-1]
            if name not in expected:
                continue
            if any(case.find(tag) is not None for tag in ("skipped", "failure", "error")):
                raise AcceptanceError("A requested system acceptance case did not pass")
            completed.add(name)
        if completed != expected:
            raise AcceptanceError("The QTest report does not prove both system acceptance cases ran")
        return sorted(completed)
    except (ET.ParseError, OSError) as error:
        raise AcceptanceError("The structured QTest acceptance report is missing or invalid") from error


def cleanup_test_dns_cache(native, *, test_started, cores_exited):
    result = {"attempted": False, "succeeded": None}
    if not test_started:
        return dict(result, reason="test_not_started")
    if not cores_exited:
        return dict(result, reason="test_core_cleanup_not_confirmed")
    result["attempted"] = True
    try:
        native.flush_dns_cache()
        result["succeeded"] = True
    except Exception as error:
        result["succeeded"] = False
        result["error"] = (str(error) if isinstance(error, AcceptanceError)
                           else f"DNS cache cleanup failed: {type(error).__name__}")
    return result


def guarded_run(configuration):
    native = Native()
    gui_tests, installation, output = (Path(configuration[key]) for key in ("gui_tests", "installation", "output"))
    validate_paths(gui_tests, installation, output)
    report = {"schema": 1, "mode": "execute", "passed": False, "failures": []}
    original = []
    runner = None
    child = None
    before_proxy = before_network = before_hashes = None
    old_test_cores = {}
    audit = output / "synthetic-proxy-audit.json"
    deadline = time.monotonic() + configuration["timeout"]
    try:
        runner = Process(native, configuration["runner"]["pid"], expected=configuration["runner"])
        original, roots = installation_processes(native, installation)
        before_proxy = proxy_snapshot(native)
        before_network = network_snapshot()
        backup = json.dumps(before_proxy, ensure_ascii=False).encode("utf-8")
        (output / "proxy-recovery.dpapi").write_bytes(native.protect(backup))
        if not runner.alive() or (output / "cancel").exists():
            raise AcceptanceError("Runner ended before host suspension")
        for process in original:
            if not runner.alive() or (output / "cancel").exists() or time.monotonic() >= deadline:
                raise AcceptanceError("Runner ended or exhausted its budget during host suspension")
            process.suspend()
        # Freeze new GUI children too, if one was spawned after the first tree
        # snapshot but before its GUI became suspended. No process-name kill.
        refreshed, refreshed_roots = installation_processes(native, installation)
        known = {process.identity["pid"]: process.identity for process in original}
        try:
            if refreshed_roots != roots:
                raise AcceptanceError("The selected installation changed during suspension")
            for process in refreshed:
                pid = process.identity["pid"]
                if pid in known:
                    if known[pid] != process.identity:
                        raise AcceptanceError("A process PID was reused during suspension")
                    process.close()
                else:
                    original.append(process)
                    process.suspend()
            refreshed = []
        finally:
            for process in refreshed:
                if process not in original:
                    process.close()
        before_hashes = config_hashes(installation)
        report["before"] = summarize(before_proxy, before_network, before_hashes, original, roots)
        write_json(output / "original-process-identities.json", [process.identity for process in original])
        old_test_cores = test_core_identities(native, gui_tests.parent)
        report["preexisting_test_directory_core_count"] = len(old_test_cores)
        environment = os.environ.copy()
        environment.update(NEKORAY_SYSTEM_ACCEPTANCE="1", NEKORAY_SYSTEM_PROXY_AUDIT=str(audit),
                           QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1", NEKORAY_TEST_CORE="ray")
        environment["PATH"] = str(gui_tests.parent) + os.pathsep + environment.get("PATH", "")
        arguments = [str(gui_tests), "systemProxyRuntimeAcceptance", "tunSystemRuntimeAcceptance", "-v1",
                     "-o", str(output / "qtest.txt") + ",txt", "-o", str(output / "qtest.xml") + ",junitxml"]
        if not runner.alive() or (output / "cancel").exists() or time.monotonic() >= deadline:
            raise AcceptanceError("Runner ended or exhausted its budget before the test could start")
        with (output / "test-process.log").open("wb") as log:
            child = subprocess.Popen(arguments, cwd=gui_tests.parent, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP)
            # Popen retains the real process handle; all timeout termination
            # below uses that handle, never a PID or image-name lookup.
            report["test_identity"] = native.identity(child._handle, child.pid)
            while child.poll() is None:
                if not runner.alive():
                    raise AcceptanceError("Outer runner exited; guardian stopped the acceptance test")
                if (output / "cancel").exists():
                    raise AcceptanceError("Acceptance cancelled; guardian is restoring the original installation")
                if time.monotonic() >= deadline:
                    raise AcceptanceError("Acceptance exceeded its time budget, including UAC")
                time.sleep(0.1)
            report["test_exit_code"] = child.returncode
            if child.returncode:
                report["failures"].append("QTest reported a failure; see the synthetic test logs")
            else:
                report["completed_test_cases"] = completed_test_cases(output / "qtest.xml")
    except Exception as error:
        report["failures"].append(str(error) if isinstance(error, AcceptanceError) else f"Host acceptance failed: {type(error).__name__}")
    finally:
        test_cores_exited = False
        if child is not None:
            try:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=10)
                    report["test_terminated_by_guardian"] = True
                cleanup_deadline = time.monotonic() + 18
                while True:
                    remaining = {key: value for key, value in test_core_identities(native, gui_tests.parent).items()
                                 if key not in old_test_cores}
                    if not remaining or time.monotonic() >= cleanup_deadline:
                        break
                    time.sleep(0.2)
                report["remaining_test_directory_core_count"] = len(remaining)
                if remaining:
                    report["failures"].append("New test-directory core processes remain after the owner-watchdog grace period")
                else:
                    test_cores_exited = True
            except Exception as error:
                report["failures"].append(f"Test child cleanup failed: {type(error).__name__}")
        # The real test may have populated the system cache with synthetic
        # DNS answers. Flush only after its cores are confirmed gone, before
        # resuming the original installation. No DNS server settings change.
        report["dns_cache_cleanup"] = cleanup_test_dns_cache(
            native, test_started=child is not None, cores_exited=test_cores_exited)
        if report["dns_cache_cleanup"]["attempted"] and not report["dns_cache_cleanup"]["succeeded"]:
            report["failures"].append(report["dns_cache_cleanup"]["error"])
        if before_proxy is not None and any(process.suspended for process in original):
            try:
                report["proxy_recovery"] = restore_proxy(native, before_proxy, audit)
                if not report["proxy_recovery"]["semantic_state_restored"]:
                    report["failures"].append("System proxy changed outside the test's owned values; it was not overwritten")
                if not report["proxy_recovery"]["registry_restored"]:
                    report["failures"].append("Proxy registry fields differ from the original snapshot")
            except Exception as error:
                report["failures"].append(str(error) if isinstance(error, AcceptanceError) else f"Proxy recovery failed: {type(error).__name__}")
        if before_hashes is not None:
            try:
                after_hashes = config_hashes(installation)
                report["original_config_unchanged"] = before_hashes == after_hashes
                if not report["original_config_unchanged"]:
                    report["failures"].append("Original configuration file hashes changed while the installation was paused")
            except Exception as error:
                report["failures"].append(f"Cannot verify original configuration hashes: {type(error).__name__}")
        # Always resume in reverse order, even if proxy recovery or hashing
        # failed. One failed resume must not prevent resuming the other handles.
        resume_failures = 0
        for process in reversed(original):
            try:
                process.resume()
                if not process.alive():
                    report["failures"].append("An original installation process exited during acceptance")
            except Exception as error:
                resume_failures += 1
                report["failures"].append(str(error) if isinstance(error, AcceptanceError) else "Original process resume failed")
            finally:
                process.close()
        report["original_processes_resumed"] = resume_failures == 0
        if runner is not None:
            runner.close()
        if before_network is not None:
            try:
                after_network = network_snapshot()
                comparison = {}
                for name, rows in before_network.items():
                    previous, current = set(rows), set(after_network[name])
                    comparison[name] = {"restored": rows == after_network[name],
                                        "added": len(current - previous), "removed": len(previous - current)}
                    if rows != after_network[name]:
                        report["failures"].append(f"Host {name} differ after test cleanup")
                report["network_recovery"] = comparison
            except Exception as error:
                report["failures"].append(str(error) if isinstance(error, AcceptanceError) else "Cannot verify final network state")
        report["passed"] = not report["failures"] and report.get("test_exit_code") == 0
        write_json(output / "result.json", report)
    return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui-tests", type=Path)
    parser.add_argument("--existing-install", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--execute", action="store_true", help="Run the system tests; otherwise only perform read-only preflight")
    parser.add_argument("--timeout", type=int, default=180, help="Total execution budget in seconds, including UAC (default: 180)")
    parser.add_argument("--guarded-run", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if os.name != "nt":
        parser.error("This acceptance runner is Windows-only")
    if arguments.guarded_run:
        return guarded_run(json.loads(arguments.guarded_run.read_text(encoding="utf-8")))
    if not all((arguments.gui_tests, arguments.existing_install, arguments.output_dir)):
        parser.error("--gui-tests, --existing-install and --output-dir are required")
    if not 30 <= arguments.timeout <= 600:
        parser.error("--timeout must be between 30 and 600 seconds")
    gui_tests, installation, output_root = (path.resolve() for path in
                                           (arguments.gui_tests, arguments.existing_install, arguments.output_dir))
    native = Native()
    try:
        validate_paths(gui_tests, installation, output_root)
        output_root.mkdir(parents=True, exist_ok=True)
        output = output_root / ("run-" + uuid.uuid4().hex)
        output.mkdir()
        native.private_directory(output)
        report = {"schema": 1, "mode": "preflight", "passed": True,
                  "summary": inspect_host(native, installation), "report_directory": str(output)}
        write_json(output / "preflight.json", report)
        if not arguments.execute:
            print(json.dumps(report, ensure_ascii=False, indent=2))
            return 0
        own = Process(native, os.getpid())
        configuration = {"gui_tests": str(gui_tests), "installation": str(installation), "output": str(output),
                         "timeout": arguments.timeout, "runner": own.identity}
        own.close()
        configuration_path = output / "guardian.json"
        write_json(configuration_path, configuration)
        with (output / "guardian.log").open("wb") as log:
            guardian = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--guarded-run", str(configuration_path)],
                                        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                        creationflags=CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP)
            try:
                while guardian.poll() is None:
                    time.sleep(0.2)
            except KeyboardInterrupt:
                (output / "cancel").touch()
                # Do not terminate the guardian: its finally block owns recovery.
                while guardian.poll() is None:
                    try:
                        time.sleep(0.2)
                    except KeyboardInterrupt:
                        pass
        result_path = output / "result.json"
        if not result_path.is_file():
            raise AcceptanceError("Recovery guardian exited without a final report; inspect the private guardian log")
        result = json.loads(result_path.read_text(encoding="utf-8"))
        result["report_directory"] = str(output)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["passed"] else 1
    except Exception as error:
        print(json.dumps({"passed": False, "error": str(error) if isinstance(error, AcceptanceError)
                          else f"Preflight could not complete: {type(error).__name__}"}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
