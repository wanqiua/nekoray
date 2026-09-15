"""Run one acceptance process tree in a Windows job, assigned before it starts."""

from __future__ import annotations

import ctypes
from ctypes import wintypes as w
from collections.abc import Callable
from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import time


@dataclass(frozen=True)
class OwnedProcess:
    pid: int
    handle: int
    created: int
    deadline: float


class StartupOnlyAcceptance(RuntimeError):
    """Startup was observed; the owned job then supplied bounded cleanup."""


def run_owned(executable: Path, arguments: list[str], cwd: Path, environment: dict[str, str], log: Path,
              timeout: int = 30, on_started: Callable[[], None] | None = None,
              observe: Callable[[OwnedProcess], None] | None = None, allow_startup_only: bool = False,
              show_window: bool = False) -> int:
    if allow_startup_only and observe is None:
        raise ValueError("Startup-only acceptance requires a successful process observer")
    if os.name != "nt":
        raise RuntimeError("Native package acceptance requires Windows")
    import msvcrt

    class StartupInfo(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("reserved", w.LPWSTR), ("desktop", w.LPWSTR), ("title", w.LPWSTR),
                    ("x", w.DWORD), ("y", w.DWORD), ("xsize", w.DWORD), ("ysize", w.DWORD),
                    ("xchars", w.DWORD), ("ychars", w.DWORD), ("fill", w.DWORD), ("flags", w.DWORD),
                    ("show", w.WORD), ("reserved_size", w.WORD), ("reserved_ptr", ctypes.c_void_p),
                    ("stdin", w.HANDLE), ("stdout", w.HANDLE), ("stderr", w.HANDLE)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("process", w.HANDLE), ("thread", w.HANDLE), ("pid", w.DWORD), ("tid", w.DWORD)]

    class BasicLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64), ("flags", w.DWORD),
                    ("working_min", ctypes.c_size_t), ("working_max", ctypes.c_size_t), ("active_limit", w.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", IoCounters), ("process_memory", ctypes.c_size_t),
                    ("job_memory", ctypes.c_size_t), ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

    class Accounting(ctypes.Structure):
        _fields_ = [("user", ctypes.c_int64), ("kernel", ctypes.c_int64), ("period_user", ctypes.c_int64),
                    ("period_kernel", ctypes.c_int64), ("page_faults", w.DWORD), ("total", w.DWORD),
                    ("active", w.DWORD), ("terminated", w.DWORD)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "CreateJobObjectW": ([ctypes.c_void_p, w.LPCWSTR], w.HANDLE),
        "SetInformationJobObject": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD], w.BOOL),
        "QueryInformationJobObject": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p], w.BOOL),
        "AssignProcessToJobObject": ([w.HANDLE, w.HANDLE], w.BOOL),
        "TerminateJobObject": ([w.HANDLE, w.UINT], w.BOOL),
        "CreateProcessW": ([w.LPCWSTR, w.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, w.BOOL, w.DWORD,
                            ctypes.c_void_p, w.LPCWSTR, ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo)], w.BOOL),
        "ResumeThread": ([w.HANDLE], w.DWORD),
        "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
        "GetExitCodeProcess": ([w.HANDLE, ctypes.POINTER(w.DWORD)], w.BOOL),
        "GetProcessTimes": ([w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4, w.BOOL),
        "TerminateProcess": ([w.HANDLE, w.UINT], w.BOOL),
        "OpenProcess": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
        "IsProcessInJob": ([w.HANDLE, w.HANDLE, ctypes.POINTER(w.BOOL)], w.BOOL),
        "CloseHandle": ([w.HANDLE], w.BOOL),
    }
    for name, (arguments_type, result_type) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes, function.restype = arguments_type, result_type

    def checked(value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())
        return value

    def owned_process_handles(job):
        capacity = 32
        while True:
            class ProcessIds(ctypes.Structure):
                _fields_ = [("assigned", w.DWORD), ("count", w.DWORD), ("ids", ctypes.c_size_t * capacity)]

            process_ids = ProcessIds()
            if kernel.QueryInformationJobObject(job, 3, ctypes.byref(process_ids), ctypes.sizeof(process_ids), None):
                break
            if ctypes.get_last_error() != 234:  # ERROR_MORE_DATA
                raise ctypes.WinError(ctypes.get_last_error())
            capacity = max(capacity * 2, process_ids.assigned)
        handles = []
        try:
            for pid in process_ids.ids[:process_ids.count]:
                handle = kernel.OpenProcess(0x100000 | 0x1000, False, pid)  # SYNCHRONIZE | QUERY_LIMITED_INFORMATION
                if not handle:
                    if ctypes.get_last_error() == 87:  # Already exited.
                        continue
                    raise ctypes.WinError(ctypes.get_last_error())
                in_job = w.BOOL()
                if not kernel.IsProcessInJob(handle, job, ctypes.byref(in_job)):
                    error = ctypes.WinError(ctypes.get_last_error())
                    kernel.CloseHandle(handle)
                    raise error
                if in_job.value:
                    handles.append(handle)
                else:
                    kernel.CloseHandle(handle)
            return handles
        except Exception:
            for handle in handles:
                kernel.CloseHandle(handle)
            raise

    job = checked(kernel.CreateJobObjectW(None, None))
    process = ProcessInfo()
    resumed = False
    try:
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        checked(kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)))
        command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(executable), *arguments]))
        block = ctypes.create_unicode_buffer("\0".join(f"{key}={value}" for key, value in sorted(environment.items(), key=lambda item: item[0].upper())) + "\0\0")
        with log.open("wb") as output, open(os.devnull, "rb") as input_stream:
            out_handle = msvcrt.get_osfhandle(output.fileno())
            in_handle = msvcrt.get_osfhandle(input_stream.fileno())
            os.set_handle_inheritable(out_handle, True)
            os.set_handle_inheritable(in_handle, True)
            startup = StartupInfo()
            startup.cb, startup.flags, startup.show = ctypes.sizeof(startup), 0x101, 1 if show_window else 0
            startup.stdin, startup.stdout, startup.stderr = in_handle, out_handle, out_handle
            checked(kernel.CreateProcessW(str(executable), command, None, None, True,
                                           0x4 | 0x400 | 0x08000000, block, str(cwd),
                                           ctypes.byref(startup), ctypes.byref(process)))
            checked(kernel.AssignProcessToJobObject(job, process.process))
            if kernel.ResumeThread(process.thread) == 0xFFFFFFFF:
                raise ctypes.WinError(ctypes.get_last_error())
            resumed = True
            deadline = time.monotonic() + timeout
            if on_started is not None:
                on_started()
            if observe is not None:
                created, exited, kernel_time, user_time = (w.FILETIME() for _ in range(4))
                checked(kernel.GetProcessTimes(process.process, ctypes.byref(created), ctypes.byref(exited),
                                               ctypes.byref(kernel_time), ctypes.byref(user_time)))
                observe(OwnedProcess(process.pid, process.process,
                                     (created.dwHighDateTime << 32) | created.dwLowDateTime, deadline))
            wait = kernel.WaitForSingleObject(process.process, max(0, int((deadline - time.monotonic()) * 1000)))
            if wait == 0x102:
                if allow_startup_only:
                    raise StartupOnlyAcceptance("Startup passed; normal exit was not confirmed and only the owned job was stopped")
                raise TimeoutError("Acceptance process exceeded its timeout; its owned job was stopped")
            if wait != 0:
                raise ctypes.WinError(ctypes.get_last_error())
            code = w.DWORD()
            checked(kernel.GetExitCodeProcess(process.process, ctypes.byref(code)))
            accounting = Accounting()
            # A normal GUI exit is required to stop its own daemons. The job is
            # a cleanup boundary, not a way to hide leaked child processes.
            for _ in range(10):
                checked(kernel.QueryInformationJobObject(job, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None))
                if not accounting.active:
                    break
                time.sleep(0.05)
            if accounting.active:
                raise RuntimeError("The acceptance process left an owned child running")
            return code.value
    finally:
        owned_handles = []
        try:
            # Closing a kill-on-close job starts asynchronous termination. Wait
            # for every owned process to exit before returning, so inherited
            # log handles cannot race with fixture inspection or cleanup.
            if process.process and not resumed:
                checked(kernel.TerminateProcess(process.process, 125))
                if kernel.WaitForSingleObject(process.process, 5000) != 0:
                    raise RuntimeError("The suspended acceptance process did not stop")
            owned_handles = owned_process_handles(job)
            checked(kernel.TerminateJobObject(job, 124))
            accounting = Accounting()
            deadline = time.monotonic() + 5
            for handle in ([process.process] if process.process else []) + owned_handles:
                milliseconds = max(0, int((deadline - time.monotonic()) * 1000))
                if kernel.WaitForSingleObject(handle, milliseconds) != 0:
                    raise RuntimeError("An owned acceptance process did not finish exiting")
            while True:
                checked(kernel.QueryInformationJobObject(job, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None))
                if not accounting.active:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("The owned acceptance process tree did not stop")
                time.sleep(0.02)
        finally:
            for handle in (*owned_handles, process.thread, process.process, job):
                if handle:
                    kernel.CloseHandle(handle)
