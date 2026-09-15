"""Native candidate/legacy GUI and TLS acceptance in disposable owned jobs.

This entry point executes native processes. Use package_acceptance.py static or
the pure mock tests while a candidate is still being prepared.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import stat
import time
import uuid

from package_acceptance import (GUI_FILENAME, check_core_manifest, clean_environment, required_portable_files,
                                json_without_duplicate_keys, prepare, relative_path, verify_archive)
from package_fixture import (assert_original_bytes, copy_fixture, digest, read_json, require_fixture, write_json)
from package_windows_gui import inspect_owned_gui
from package_windows_process import StartupOnlyAcceptance, run_owned

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]
DEFAULT_LEGACY = REPOSITORY / "build/core-upgrade-check/reference/nekoray"
OMIT_DIRECTORIES = {"config", "profiles", "groups", "routes", "routes_box", "logs", "log", ".git", "__pycache__"}


def plain(path: Path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ValueError("Links and reparse points cannot be used as a native payload")
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
        raise ValueError("A native payload contains a special file")


def runtime_files(root: Path, *, exclude_configuration: bool):
    plain(root)
    result = []
    pending = [root]
    while pending:
        directory = pending.pop()
        for path in sorted(directory.iterdir()):
            name = path.name.casefold()
            # Do not descend into, hash, parse or copy any legacy configuration.
            omitted = name in OMIT_DIRECTORIES or name.endswith((".log", ".dmp", ".dpapi", ".pdb"))
            if omitted:
                if exclude_configuration:
                    continue
                raise ValueError("A verified native payload contains configuration or development artifacts")
            plain(path)
            if path.is_dir():
                pending.append(path)
            else:
                result.append(path.relative_to(root).as_posix())
    return sorted(result)


def stage_verified_payload(source: Path, static_report: Path, destination: Path) -> dict:
    if destination.exists():
        raise ValueError("A native payload copy requires a new directory")
    report = read_json(static_report)
    if (report.get("source_archive_verified") is not True or
        report.get("go_build_info_verified") is not True or report.get("go_module_notices_verified") is not True or
        Path(report.get("payload", "")).resolve() != source.resolve() or
        not report.get("manifest_sha256")):
        raise ValueError("Run the current static verifier for this exact payload before native acceptance")
    plain(source / "package-manifest.json")
    if digest(source / "package-manifest.json") != report["manifest_sha256"]:
        raise ValueError("The verified payload manifest changed")
    manifest = json_without_duplicate_keys((source / "package-manifest.json").read_bytes())
    check_core_manifest(manifest)
    records = {}
    folded = set()
    for item in manifest["files"]:
        name = relative_path(item["path"])
        if name.casefold() in folded or name == "package-manifest.json":
            raise ValueError("Duplicate verified payload manifest record")
        folded.add(name.casefold())
        records[name] = item
    if not required_portable_files(manifest) <= records.keys():
        raise ValueError("The verified payload is missing required runtime files")
    names = runtime_files(source, exclude_configuration=False)
    if set(names) != set(records) | {"package-manifest.json"}:
        raise ValueError("The extracted payload changed after static verification")
    for name, item in records.items():
        path = source / name
        if path.stat().st_size != item["bytes"] or digest(path).lower() != item["sha256"].lower():
            raise ValueError(f"The extracted payload changed after verification: {name}")
    destination.mkdir(parents=True)
    for name in names:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, target)
    return {**report, "payload": str(destination.resolve()), "copied_from_verified_payload": True}


def stage_legacy_reference(source: Path, destination: Path) -> dict:
    if not source.is_dir() or destination.exists():
        raise ValueError("Legacy acceptance requires an existing reference and a new destination")
    if source.resolve() == destination.resolve() or source.resolve() in destination.resolve().parents:
        raise ValueError("The legacy copy cannot be placed inside its source")
    names = runtime_files(source, exclude_configuration=True)
    if "nekoray.exe" not in names:
        raise ValueError("The legacy reference has no nekoray.exe")
    destination.mkdir(parents=True)
    inventory = []
    for name in names:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, target)
        inventory.append({"path": name, "sha256": digest(target)})
    return {"source": str(source.resolve()), "payload": str(destination.resolve()), "files": inventory,
            "source_configuration_read": False, "source_configuration_copied": False}


def native_environment():
    environment = clean_environment()
    environment = {key: value for key, value in environment.items()
                   if not key.upper().startswith(("NEKORAY_", "NKR_", "XRAY_", "V2RAY_", "SING_BOX_"))
                   and key.upper() not in {"SSL_CERT_FILE", "SSL_CERT_DIR"}}
    environment["QT_QPA_PLATFORM"] = "windows"
    return environment


def required_gui_modules(payload: Path):
    major = "6" if (payload / "Qt6Core.dll").is_file() else "5"
    names = [f"Qt{major}{component}.dll" for component in ("Core", "Gui", "Widgets", "Network", "Svg")]
    names.append("qwindows.dll")
    for name in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        if (payload / name).is_file():
            names.append(name)
    return names


def gui_control_executable(environment: dict) -> Path:
    system_root = environment.get("SystemRoot", environment.get("SYSTEMROOT", r"C:\Windows"))
    return Path(system_root) / "System32/WindowsPowerShell/v1.0/powershell.exe"


def require_file(path: Path, label: str):
    if not path.is_file():
        raise ValueError(f"Missing {label}: {path}")
    plain(path)
    if path.stat().st_size == 0:
        raise ValueError(f"Empty {label}: {path}")


def gui_executable(payload: Path) -> Path:
    branded = payload / GUI_FILENAME
    return branded if branded.is_file() else payload / "nekoray.exe"


def validate_native_inputs(payload: Path, legacy_payload: Path, smoke: Path, powershell: Path, environment: dict):
    # Fail before starting any GUI if a later acceptance step cannot run.
    for path, label in ((smoke, "TLS smoke executable"), (powershell, "PowerShell 7 executable"),
                        (gui_control_executable(environment), "Windows PowerShell GUI control executable"),
                        (HERE / "package_gui_control.ps1", "GUI control script"),
                        (HERE / "package_tls_fixture.ps1", "TLS fixture script")):
        require_file(path, label)
    for root in (payload, legacy_payload):
        require_file(gui_executable(root), "GUI executable")
        files = runtime_files(root, exclude_configuration=False)
        present = {Path(name).name.casefold() for name in files}
        missing = {name.casefold() for name in required_gui_modules(root)} - present
        if missing:
            raise ValueError("A GUI payload is missing runtime modules: " + ", ".join(sorted(missing)))


def stage_native_fixture(baseline: Path, destination: Path):
    assert_original_bytes(baseline)
    copy_fixture(baseline, destination)
    marker = require_fixture(destination)
    for name in ("nekoray.json", "nekobox.json"):
        path = destination / "config/groups" / name
        settings = read_json(path)
        if (settings.get("remember_enable", True) or settings.get("remember_id", 0) >= 0 or
            settings.get("spmode2") != [] or settings.get("sub_auto_update", 30) > 0 or
            any(settings.get(key) for key in ("hk_mw", "hk_group", "hk_route", "hk_spmenu"))):
            raise ValueError("The native fixture can enable a connection or system mode")
        settings["start_minimal"] = False  # Test copy only: expose an inspectable native main window.
        write_json(path, settings)
    return marker


def run_gui(payload: Path, baseline: Path, work: Path, label: str, environment: dict,
            on_started, *, legacy: bool, timeout: int) -> dict:
    fixture = work / (label + "-config")
    stage_native_fixture(baseline, fixture)
    executable = gui_executable(payload)
    result = {"scope": "native-gui", "executable_sha256": digest(executable),
              "configuration": "synthetic-pre-upgrade-copy", "normal_exit": False}

    def observe(process):
        powershell = gui_control_executable(environment)
        system_directory = powershell.parents[2]
        observation = inspect_owned_gui(process, payload, required_gui_modules(payload),
                                        legacy_system_directory=system_directory if legacy else None)
        result.update(window_ready=True, runtime_modules=observation["runtime_modules"])
        action_report = work / (label + "-ui.json")
        arguments = ["-NoProfile", "-NonInteractive", "-STA", "-ExecutionPolicy", "Bypass", "-File",
                     str(HERE / "package_gui_control.ps1"), "-TargetPid", str(process.pid),
                     "-Created", str(process.created), "-Window", str(observation["window"]),
                     "-Executable", str(executable.resolve()), "-Report", str(action_report)]
        remaining = max(1, min(8, int(process.deadline - time.monotonic())))
        code = run_owned(powershell, arguments, work, environment, work / (label + "-ui.log"), timeout=remaining)
        if code or not action_report.is_file():
            raise RuntimeError("The owned GUI control helper did not finish")
        action = read_json(action_report)
        if not action.get("identity_verified"):
            raise RuntimeError("The GUI control helper could not verify the retained process identity")
        groups_visible = set(action.get("groups_visible", [])) == {"合成订阅组", "手动测试组"}
        result.update(synthetic_groups_visible=groups_visible, exit_menu_invoked=bool(action.get("exit_invoked")))
        if not groups_visible and not legacy:
            raise RuntimeError("The native candidate GUI did not expose the synthetic fixture groups")
        if action.get("exit_reason"):
            result["exit_note"] = action["exit_reason"]

    try:
        code = run_owned(executable, ["-appdata", str(fixture.resolve()), "-many"], payload, environment,
                         work / (label + ".log"), timeout=timeout, on_started=on_started,
                         observe=observe, allow_startup_only=legacy, show_window=True)
        if code:
            raise RuntimeError(f"The native {label} GUI exited with code {code}")
        if not result.get("exit_menu_invoked"):
            raise RuntimeError("The GUI exited without completing the owned normal Exit action")
        result.update(result="passed", normal_exit=True)
    except StartupOnlyAcceptance:
        # Only the legacy path opts into this weaker boundary. The job wrapper
        # has already confirmed every owned process stopped before returning.
        if not legacy or not result.get("window_ready"):
            result.update(result="failed", failure="Candidate normal exit was not confirmed")
            raise
        result.update(result="startup-only", scope="startup-only", cleanup="owned-job-confirmed",
                      limitation="Old-GUI startup and packaged DLLs verified; normal save/exit was not verified")
    except Exception as error:
        result.update(result="failed", failure=str(error))
        raise
    finally:
        try:
            assert_original_bytes(baseline)
        except Exception as error:
            result.update(result="failed", failure=str(error))
            raise
        finally:
            write_json(work / (label + "-result.json"), result)
    return result


def run_tls(payload: Path, smoke: Path, work: Path, environment: dict, powershell: Path, on_started):
    staged_smoke = payload / "nekoray_package_smoke.exe"
    plain(smoke)
    shutil.copy2(smoke, staged_smoke)
    records = []
    for name, dns_name in (("trusted-localhost", "localhost"), ("hostname-mismatch", "mismatch.example.test")):
        certificate, key = work / (name + ".pem"), work / (name + "-key.pem")
        code = run_owned(powershell, ["-NoProfile", "-NonInteractive", "-File", str(HERE / "package_tls_fixture.ps1"),
                                     "-Certificate", str(certificate), "-PrivateKey", str(key), "-DnsName", dns_name],
                         work, environment, work / (name + "-certificate.log"), timeout=15, on_started=on_started)
        if code or not certificate.is_file() or not key.is_file():
            raise RuntimeError("Temporary TLS fixture generation failed")
        log = work / (name + "-tls.log")
        code = run_owned(staged_smoke, [str(certificate), str(key)], payload, environment, log,
                         timeout=15, on_started=on_started)
        output = log.read_text(encoding="utf-8", errors="replace")
        if name == "trusted-localhost":
            if code != 0 or "HTTPS certificate verification and 204 response passed." not in output:
                raise RuntimeError("Packaged TLS did not verify the localhost certificate and return HTTP 204")
            records.append({"case": name, "result": "passed", "http_status": 204,
                            "runtime_sources": "checked by package_smoke"})
        else:
            if code != 8 or "SSL handshake failed" not in output:
                raise RuntimeError("Packaged TLS did not reject the certificate hostname mismatch")
            records.append({"case": name, "result": "passed", "tls_handshake_rejected": True})
    return {"result": "passed", "smoke_sha256": digest(staged_smoke), "cases": records,
            "certificate_store_modified": False}


def native_acceptance(work: Path, *, smoke: Path, legacy_source: Path, archive: Path | None = None,
                      verified_payload: Path | None = None, static_report: Path | None = None,
                      powershell: Path | None = None, timeout: int = 30):
    report = prepare(work)
    report.update(phase="native", checkpoints=[], system_modes_enabled=False)
    started = lambda: report.update(native_executed=True)
    try:
        if not 15 <= timeout <= 120:
            raise ValueError("The per-GUI timeout must be between 15 and 120 seconds")
        if bool(archive) == bool(verified_payload):
            raise ValueError("Select one candidate archive or verified payload")
        if archive:
            candidate = verify_archive(archive, work / "candidate")
        else:
            if static_report is None:
                raise ValueError("A verified payload requires its current static-verification.json")
            candidate = stage_verified_payload(verified_payload, static_report, work / "candidate/nekoray")
        report["candidate"] = candidate
        payload = Path(candidate["payload"])
        report["legacy"] = stage_legacy_reference(legacy_source, work / "legacy/nekoray")
        if powershell is None:
            found = shutil.which("pwsh.exe") or shutil.which("pwsh")
            if not found:
                raise ValueError("PowerShell 7 is required to generate temporary TLS certificates")
            powershell = Path(found)
        environment = native_environment()
        validate_native_inputs(payload, Path(report["legacy"]["payload"]), smoke, powershell, environment)

        def run_case(name, *, legacy):
            label = ("legacy-" if legacy else "candidate-") + name
            checkpoint = {"core_source": name, "legacy": legacy, "result": "started"}
            report["checkpoints"].append(checkpoint)
            baseline = work / (("rollback-" if legacy else "baseline-") + name)
            try:
                checkpoint.update(run_gui(Path(report["legacy"]["payload"]) if legacy else payload,
                                          baseline, work, label, environment, started, legacy=legacy, timeout=timeout))
            finally:
                if (work / (label + "-result.json")).is_file():
                    checkpoint.update(read_json(work / (label + "-result.json")))

        for name in ("ray", "box"):
            run_case(name, legacy=False)
        report["tls"] = run_tls(payload, smoke, work, environment, powershell, started)
        for name in ("ray", "box"):
            run_case(name, legacy=True)
        report.update(result="passed", legacy_binary_launch="performed on synthetic pre-upgrade copies",
                      legacy_normal_exit_verified=all(item.get("normal_exit") for item in report["checkpoints"] if item["legacy"]))
    except Exception as error:
        report.update(result="failed", failure=str(error))
        raise
    finally:
        write_json(work / "native-acceptance.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    candidate = parser.add_mutually_exclusive_group(required=True)
    candidate.add_argument("--archive", type=Path)
    candidate.add_argument("--verified-payload", type=Path)
    parser.add_argument("--static-report", type=Path)
    parser.add_argument("--smoke", type=Path, required=True)
    parser.add_argument("--legacy-source", type=Path, default=DEFAULT_LEGACY)
    parser.add_argument("--powershell", type=Path, help="PowerShell 7 executable; defaults to pwsh on the preparation PATH")
    parser.add_argument("--work", type=Path)
    parser.add_argument("--timeout", type=int, default=30, help="Per-GUI timeout including observation and normal exit")
    arguments = parser.parse_args()
    if not 15 <= arguments.timeout <= 120:
        parser.error("--timeout must be between 15 and 120 seconds")
    work = arguments.work or REPOSITORY / "build/package-native-acceptance" / uuid.uuid4().hex
    result = native_acceptance(work.resolve(), archive=arguments.archive.resolve() if arguments.archive else None,
                               verified_payload=arguments.verified_payload.resolve() if arguments.verified_payload else None,
                               static_report=arguments.static_report.resolve() if arguments.static_report else None,
                               smoke=arguments.smoke.resolve(), legacy_source=arguments.legacy_source.resolve(),
                               powershell=arguments.powershell.resolve() if arguments.powershell else None,
                               timeout=arguments.timeout)
    print(json.dumps({"work": str(work.resolve()), **result}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
