"""Read only the retained acceptance process; never discovers a GUI by name."""

from __future__ import annotations

import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import time

from package_fixture import digest
from package_windows_process import OwnedProcess


class RuntimeNotReady(RuntimeError):
    pass


def verify_runtime_sources(paths, payload: Path, required, *, legacy_system_directory: Path | None = None):
    root = os.path.normcase(os.path.realpath(payload))
    protected = {"qwindows.dll", "qopensslbackend.dll", "qschannelbackend.dll", "qsvgicon.dll",
                 "qsvg.dll", "qjpeg.dll", "qgif.dll", "qwindowsvistastyle.dll", "qtuiotouchplugin.dll",
                 "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
    crt_names = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
    modules = {}
    for value in paths:
        path = Path(value)
        name = path.name.casefold()
        if not (name.startswith(("qt5", "qt6", "libssl", "libcrypto")) or name in protected):
            continue
        absolute = os.path.normcase(os.path.realpath(path))
        try:
            local = os.path.commonpath((absolute, root)) == root
        except ValueError:
            local = False
        # The original 3.26 reference did not bundle MSVC CRT DLLs. Its old-GUI
        # launch may use the installed System32 CRT, and reports that dependency
        # explicitly. The candidate always requires its own packaged CRT.
        legacy_system = (not local and legacy_system_directory is not None and name in crt_names and
                         os.path.normcase(os.path.realpath(path.parent)) ==
                         os.path.normcase(os.path.realpath(legacy_system_directory)))
        if not local and not legacy_system:
            raise RuntimeError(f"Loaded runtime is outside the isolated payload: {path.name}")
        modules[name] = {"name": path.name, "origin": "system-crt" if legacy_system else "payload",
                         "path": str(path) if legacy_system else os.path.relpath(absolute, root).replace("\\", "/"),
                         "sha256": digest(path)}
    missing = {name.casefold() for name in required} - modules.keys()
    if missing:
        raise RuntimeNotReady("Required GUI modules have not loaded: " + ", ".join(sorted(missing)))
    return sorted(modules.values(), key=lambda item: item["name"].casefold())


class GuiInspector:
    def __init__(self):
        if os.name != "nt":
            raise RuntimeError("Native GUI inspection requires Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.window_callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        signatures = [
            (self.kernel, "WaitForSingleObject", [w.HANDLE, w.DWORD], w.DWORD),
            (self.psapi, "EnumProcessModulesEx", [w.HANDLE, ctypes.POINTER(w.HMODULE), w.DWORD,
                                                   ctypes.POINTER(w.DWORD), w.DWORD], w.BOOL),
            (self.psapi, "GetModuleFileNameExW", [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD], w.DWORD),
            (self.user, "EnumWindows", [self.window_callback, w.LPARAM], w.BOOL),
            (self.user, "GetWindowThreadProcessId", [w.HWND, ctypes.POINTER(w.DWORD)], w.DWORD),
            (self.user, "GetClassNameW", [w.HWND, w.LPWSTR, ctypes.c_int], ctypes.c_int),
            (self.user, "GetWindowTextW", [w.HWND, w.LPWSTR, ctypes.c_int], ctypes.c_int),
            (self.user, "IsWindowVisible", [w.HWND], w.BOOL),
        ]
        for library, name, arguments, result in signatures:
            function = getattr(library, name)
            function.argtypes, function.restype = arguments, result

    def alive(self, process):
        result = self.kernel.WaitForSingleObject(process.handle, 0)
        if result not in (0, 258):
            raise ctypes.WinError(ctypes.get_last_error())
        return result == 258

    def modules(self, process):
        count = 256
        while True:
            values = (w.HMODULE * count)()
            needed = w.DWORD()
            if not self.psapi.EnumProcessModulesEx(process.handle, values, ctypes.sizeof(values), ctypes.byref(needed), 3):
                raise ctypes.WinError(ctypes.get_last_error())
            if needed.value <= ctypes.sizeof(values):
                break
            count = (needed.value + ctypes.sizeof(w.HMODULE) - 1) // ctypes.sizeof(w.HMODULE)
        result = []
        for module in values[:needed.value // ctypes.sizeof(w.HMODULE)]:
            path = ctypes.create_unicode_buffer(32768)
            if not self.psapi.GetModuleFileNameExW(process.handle, module, path, len(path)):
                raise ctypes.WinError(ctypes.get_last_error())
            result.append(path.value)
        return result

    def window(self, process):
        found = []

        def visit(handle, _):
            pid = w.DWORD()
            self.user.GetWindowThreadProcessId(handle, ctypes.byref(pid))
            if pid.value != process.pid:
                return True
            name, title = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(1024)
            self.user.GetClassNameW(handle, name, len(name))
            self.user.GetWindowTextW(handle, title, len(title))
            if (self.user.IsWindowVisible(handle) and name.value.startswith("Qt") and
                any(word in title.value.casefold() for word in ("nekoray", "nekobox"))):
                found.append(int(handle))
            return True

        callback = self.window_callback(visit)
        if not self.user.EnumWindows(callback, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        return found[0] if found else None


def inspect_owned_gui(process: OwnedProcess, payload: Path, required, *, legacy_system_directory: Path | None = None) -> dict:
    inspector = GuiInspector()
    deadline = min(process.deadline, time.monotonic() + 12)
    while time.monotonic() < deadline:
        if not inspector.alive(process):
            raise RuntimeError("The owned GUI exited before its window and packaged runtime were ready")
        window = inspector.window(process)
        if window:
            try:
                modules = verify_runtime_sources(inspector.modules(process), payload, required,
                                                 legacy_system_directory=legacy_system_directory)
                return {"pid": process.pid, "created": process.created, "window": window,
                        "runtime_modules": modules, "window_ready": True}
            except RuntimeNotReady:
                pass
        time.sleep(0.1)
    raise RuntimeError("The owned GUI did not expose a main window and packaged runtime within 12 seconds")
