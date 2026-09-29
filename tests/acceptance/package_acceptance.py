"""Verify a portable candidate and its synthetic configuration lifecycle.

The default prepare/static commands do not execute a GUI, core, or system tool.
The explicit lifecycle command runs test-only drivers in a fresh extracted copy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import uuid
import zipfile

from package_fixture import (assert_original_bytes, assert_retained, copy_fixture, create_fixture,
                             digest, read_json, stable_snapshot, write_json)
from package_go_build_info import verify_go_metadata

XRAY_REVISION = "e9639bd461dcff1c91e26a71a8752fa42d51268d"
PRODUCT_NAME = "NekoRay_BY_WanQiu"
GUI_FILENAME = PRODUCT_NAME + ".exe"
REQUIRED_FILES = {
    "nekoray.exe", "nekoray_core.exe", "nekobox_core.exe", "core-versions.json",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Svg.dll",
    "platforms/qwindows.dll", "tls/qopensslbackend.dll", "libcrypto-3-x64.dll", "libssl-3-x64.dll",
    "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "qt.conf", "LICENSE", "README.md",
    "geoip.dat", "geosite.dat", "geoip.db", "geosite.db", "licenses/README.txt",
    "licenses/go-modules.json", "licenses/nekoray_core-build-info.txt", "licenses/nekobox_core-build-info.txt",
    "docs/Dynamic_Core_Implementation.md",
}
REQUIRED_SOURCE_FILES = {
    "BUILD-SOURCE.json", "CMakeLists.txt", "libs/package_windows.ps1", "libs/package_source_inventory.ps1",
    "libs/prepare_package_notices.ps1",
    "fmt/AnyTLSBean.cpp", "fmt/AnyTLSBean.hpp", "fmt/SnellBean.cpp", "fmt/SnellBean.hpp", "fmt/QUICBean.hpp",
    "fmt/AbstractBean.cpp", "fmt/includes.h", "fmt/Bean2CoreObj_box.cpp", "fmt/Bean2CoreObj_ray.cpp",
    "fmt/Bean2External.cpp", "fmt/Bean2Link.cpp", "fmt/Link2Bean.cpp",
    "db/ProxyEntity.hpp", "db/Database.cpp", "db/CoreCapabilities.cpp", "db/CoreCapabilities.hpp",
    "sub/GroupUpdater.cpp", "ui/edit/dialog_edit_profile.cpp", "ui/edit/edit_anytls.cpp", "ui/edit/edit_anytls.h",
    "ui/edit/edit_snell.cpp", "ui/edit/edit_snell.h", "ui/edit/edit_quic.cpp", "ui/edit/edit_quic.h", "ui/edit/edit_quic.ui",
    "tests/gui_regression_test.cpp", "tests/new_protocol_regression_test.hpp", "tests/hysteria2_editor_test.hpp",
    "tests/protocol_gui_runtime_test.hpp", "tests/core_capabilities_test.cpp", "tests/acceptance/new-protocols.md",
    "docs/Dynamic_Core_Implementation.md",
    "go/cmd/nekoray_core/go.mod", "go/cmd/nekoray_core/go.sum", "go/cmd/nekoray_core/legacy_protocol_test.go",
    "go/cmd/nekoray_core/protect_bindinterface_windows.go", "go/cmd/nekoray_core/protect_bindinterface_windows_test.go",
    "go/cmd/nekobox_core/go.mod", "go/cmd/nekobox_core/go.sum", "go/cmd/nekobox_core/capabilities.go",
    "go/cmd/nekobox_core/capabilities_quic.go", "go/cmd/nekobox_core/capabilities_test.go",
    "go/cmd/nekobox_core/protocol_peers_test.go", "go/cmd/nekobox_core/protocol_gui_fixture_test.go",
    "go/cmd/nekobox_core/protocol_routing_test.go", "go/cmd/nekobox_core/probe_network_test.go",
    "go/cmd/nekobox_core/hysteria2_pin.go", "go/cmd/nekobox_core/hysteria2_pin_noquic.go",
    "go/cmd/nekobox_core/hysteria2_pin_test.go", "go/cmd/nekobox_core/block_routing_test.go",
    "go/cmd/nekoray_core/vless_xudp_test.go", "tests/release_0_1_regression_test.hpp",
    "go/grpc_server/go.mod", "go/grpc_server/go.sum", "go/grpc_server/grpc.go", "go/grpc_server/capabilities.go",
    "go/grpc_server/fulltest.go", "go/grpc_server/failures.go", "go/grpc_server/auth/auth.go",
    "go/grpc_server/probe/url.go", "go/grpc_server/probe/tcp.go", "go/grpc_server/gen/libcore.proto",
    "go/grpc_server/gen/libcore.pb.go", "go/grpc_server/gen/libcore_grpc.pb.go",
    "tests/acceptance/package_gui_probe.cpp", "tests/acceptance/package_smoke.cpp",
    "tests/acceptance/package_acceptance.py", "tests/acceptance/package_acceptance_test.py",
    "tests/acceptance/package_go_build_info.py", "tests/acceptance/package_go_build_info_test.py",
    "tests/acceptance/package_native_acceptance.py", "tests/acceptance/package_native_acceptance_test.py",
    "tests/acceptance/package_fixture.py", "tests/acceptance/package_windows_process.py",
    "tests/acceptance/package_windows_gui.py", "tests/acceptance/package_gui_control.ps1",
    "tests/acceptance/package_tls_fixture.ps1", "tests/acceptance/package_targets.cmake",
}


def json_without_duplicate_keys(data: bytes):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique)


def relative_path(value: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value or any(char in value for char in ':*?<>|"\x00'):
        raise ValueError("Invalid package path")
    parts = value.split("/")
    for part in parts:
        if part in ("", ".", "..") or part.endswith((".", " ")) or re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part):
            raise ValueError("Package path is not a plain relative Windows path")
    return value


def check_core_manifest(manifest: dict):
    if "product" in manifest and (manifest["product"] != PRODUCT_NAME or
        not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", manifest.get("gui_version", ""))):
        raise ValueError("Unexpected product name or version")
    if manifest.get("gui_base") != "3.26":
        raise ValueError("Unexpected GUI base")
    cores = manifest["cores"]
    xray, box = cores["nekoray_core"], cores["nekobox_core"]
    if (xray.get("version") != "v1.260327.0" or
        xray.get("source_module") != "github.com/wanqiua/Xray-core" or
        xray.get("source_version") != "v0.0.0-20260928091204-e9639bd461dc" or
        xray.get("source_revision") != XRAY_REVISION or
        box.get("version", "").removeprefix("v") != "1.14.0" or
        box.get("source_module") != "github.com/sagernet/sing-box"):
        raise ValueError("Candidate does not preserve the required compatible core versions and sources")


def required_portable_files(manifest: dict) -> set[str]:
    if manifest.get("product") == PRODUCT_NAME:
        if manifest.get("gui_executable") != GUI_FILENAME:
            raise ValueError("Unexpected branded GUI executable")
        return (REQUIRED_FILES - {"nekoray.exe"}) | {GUI_FILENAME}
    return REQUIRED_FILES


def archive_root(zipped: zipfile.ZipFile) -> str:
    roots = {relative_path(info.filename.rstrip("/")).split("/")[0] for info in zipped.infolist()}
    if len(roots) != 1 or not roots <= {"nekoray", PRODUCT_NAME}:
        raise ValueError("Unexpected archive root")
    return roots.pop()


def verify_source_archive(archive: Path):
    with zipfile.ZipFile(archive) as zipped:
        root = archive_root(zipped)
        files = set()
        seen = set()
        total = 0
        for info in zipped.infolist():
            name = relative_path(info.filename.rstrip("/"))
            if name != root and not name.startswith(root + "/"):
                raise ValueError("Unexpected source archive root")
            if name.casefold() in seen:
                raise ValueError("Duplicate source archive path")
            seen.add(name.casefold())
            relative = name.removeprefix(root + "/")
            parts = relative.lower().split("/")
            directories = parts if info.is_dir() else parts[:-1]
            if ((directories and directories[0] in {"config", "out"}) or
                any(part in {"deployment", "__pycache__", ".git", ".venv", "node_modules"} or
                    re.fullmatch(r"build.*|cmake-build-.*", part) for part in directories) or
                parts[-1] == ".git" or
                relative.lower().endswith((".log", ".dmp", ".dpapi", ".pyc", ".pyo", ".pdb", ".obj", ".o", ".exe", ".dll", ".ilk", ".idb", ".tlog"))):
                raise ValueError("Source archive contains runtime, configuration or cache artifacts")
            if stat.S_IFMT(info.external_attr >> 16) not in (0, stat.S_IFREG, stat.S_IFDIR):
                raise ValueError("Source archive contains a link or special file")
            total += info.file_size
            if total > 4 * 1024**3:
                raise ValueError("Source archive exceeds the acceptance size limit")
            if not info.is_dir():
                files.add(relative)
        if not REQUIRED_SOURCE_FILES <= files:
            raise ValueError("Source archive is missing build or newly added implementation files: " +
                             ", ".join(sorted(REQUIRED_SOURCE_FILES - files)))
        if zipped.testzip() is not None:
            raise ValueError("Source archive failed its CRC check")
        source = json_without_duplicate_keys(zipped.read(root + "/BUILD-SOURCE.json"))
        if not source.get("includes_working_tree_changes") or not re.fullmatch(r"[0-9a-f]{40}", source.get("base_revision", "")):
            raise ValueError("Source archive does not identify its working-tree snapshot")


def verify_archive(archive: Path, destination: Path, require_source: bool = True) -> dict:
    if destination.exists():
        raise ValueError("Archive extraction requires a new directory")
    with zipfile.ZipFile(archive) as zipped:
        root = archive_root(zipped)
        entries = {}
        normalized = set()
        total_bytes = 0
        for info in zipped.infolist():
            name = relative_path(info.filename.rstrip("/"))
            if name != root and not name.startswith(root + "/"):
                raise ValueError("Unexpected archive root")
            if name.casefold() in normalized:
                raise ValueError("Duplicate or case-colliding ZIP path")
            normalized.add(name.casefold())
            file_type = stat.S_IFMT(info.external_attr >> 16)
            if file_type not in (0, stat.S_IFREG, stat.S_IFDIR):
                raise ValueError("Archive contains a link or special file")
            total_bytes += info.file_size
            if total_bytes > 4 * 1024**3:
                raise ValueError("Portable archive exceeds the acceptance size limit")
            if not info.is_dir():
                if name == root:
                    raise ValueError("Archive root must be a directory")
                entries[name.removeprefix(root + "/")] = info
        if "package-manifest.json" not in entries:
            raise ValueError("Missing package manifest")
        manifest = json_without_duplicate_keys(zipped.read(entries["package-manifest.json"]))
        check_core_manifest(manifest)
        records = {}
        record_names = set()
        for entry in manifest["files"]:
            name = relative_path(entry["path"])
            if name.casefold() in record_names or name == "package-manifest.json":
                raise ValueError("Duplicate or self-referential manifest path")
            record_names.add(name.casefold())
            if type(entry["bytes"]) is not int or entry["bytes"] < 0 or not re.fullmatch(r"[0-9a-fA-F]{64}", entry["sha256"]):
                raise ValueError("Invalid manifest length or hash")
            records[name] = entry
        if set(entries) != set(records) | {"package-manifest.json"}:
            raise ValueError("Archive file set differs from its manifest")
        required = required_portable_files(manifest)
        if not required <= set(records):
            raise ValueError("Required portable files are missing: " + ", ".join(sorted(required - set(records))))
        for name, entry in records.items():
            parts = name.split("/")
            if (parts[0].casefold() in {"config", "profiles", "groups", "routes", "routes_box", "build", "tests"} or
                name.lower().endswith((".pdb", ".log")) or
                Path(name).name.lower() in {"updater.exe", "qt6test.dll", "package_smoke.exe", "nekoray_package_probe.exe", "nekoray_package_smoke.exe"} or
                Path(name).name.lower().endswith("_tests.exe")):
                raise ValueError("Development or user configuration data was packaged")
            info = entries[name]
            if info.file_size != entry["bytes"]:
                raise ValueError(f"Manifest length mismatch: {name}")
            with zipped.open(info) as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != entry["sha256"].lower():
                    raise ValueError(f"Manifest hash mismatch: {name}")
        core_manifest = json_without_duplicate_keys(zipped.read(entries["core-versions.json"]))
        check_core_manifest(core_manifest)
        if core_manifest.get("target_os") != "windows" or core_manifest.get("target_arch") != "amd64":
            raise ValueError("Core payload is not Windows amd64")
        if manifest["cores"] != core_manifest["cores"]:
            raise ValueError("Package and core manifests disagree")
        if any(manifest.get(key) != core_manifest.get(key) for key in ("product", "gui_version")):
            raise ValueError("Package and cores have different product versions")
        for core in ("nekoray_core", "nekobox_core"):
            if records[core + ".exe"]["sha256"].lower() != core_manifest["cores"][core]["sha256"].lower():
                raise ValueError("Core binary hash differs from its provenance")
        go_metadata = verify_go_metadata(lambda name: zipped.read(entries[name]), set(entries), core_manifest["cores"],
                                         json_without_duplicate_keys(zipped.read(entries["licenses/go-modules.json"])))
        source_verified = False
        if require_source:
            source = manifest["source"]
            name = relative_path(source["archive"])
            if "/" in name:
                raise ValueError("Source archive must be beside the portable archive")
            if digest(archive.parent / name) != source["sha256"].lower():
                raise ValueError("Source archive hash mismatch")
            verify_source_archive(archive.parent / name)
            source_verified = True
        # All names, sizes and content hashes were checked before any extraction.
        destination.mkdir(parents=True)
        payload = destination / root
        for name, info in entries.items():
            target = payload / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with zipped.open(info) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
    return {"archive": archive.name, "archive_sha256": digest(archive), "files_verified": len(records),
            "manifest_sha256": digest(payload / "package-manifest.json"),
            "source_archive_verified": source_verified, "payload": str(payload.resolve()),
            "xray": "wanqiua 26.3.27-matsuri.1", "sing_box": "1.14.0", "native_executed": False, **go_metadata}


def clean_environment() -> dict[str, str]:
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith(("QT_", "QML", "OPENSSL_", "NEKORAY_TEST_", "NKR_FROM_"))
                   and key.upper() not in {"PATH", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY"}}
    system_root = environment.get("SystemRoot", environment.get("SYSTEMROOT", r"C:\Windows"))
    environment["PATH"] = f"{system_root}\\System32;{system_root};{system_root}\\System32\\Wbem"
    environment["QT_FORCE_STDERR_LOGGING"] = "1"
    return environment


def prepare(work: Path) -> dict:
    work.mkdir(parents=True, exist_ok=False)
    fixtures = {}
    for core, name in ((0, "ray"), (1, "box")):
        baseline = work / ("baseline-" + name)
        create_fixture(baseline, core)
        copy_fixture(baseline, work / ("upgrade-" + name))
        copy_fixture(baseline, work / ("rollback-" + name))
        assert_original_bytes(baseline)
        assert_original_bytes(work / ("rollback-" + name))
        fixtures[name] = str(baseline.resolve())
    report = {"phase": "prepared", "synthetic_fixtures": fixtures, "native_executed": False,
              "package_built": False, "real_user_configuration_accessed": False}
    write_json(work / "preparation.json", report)
    return report


def lifecycle(work: Path, archive: Path, probe: Path) -> dict:
    from package_windows_process import run_owned
    report = prepare(work)
    report.update(phase="lifecycle", checkpoints=[],
                  legacy_binary_launch="not performed; backup restoration is verified separately from old-GUI runtime acceptance")
    try:
        verified = verify_archive(archive, work / "candidate")
        report["archive"] = verified
        payload = Path(verified["payload"])
        staged_probe = payload / "nekoray_package_probe.exe"
        shutil.copy2(probe, staged_probe)
        run_ids = [read_json(work / ("baseline-" + name) / "package-fixture.json")["run_id"] for name in ("ray", "box")]
        write_json(payload / "package-acceptance-owned.json", {"kind": "nekoray-package-acceptance-payload", "fixture_run_ids": run_ids})
        for name in ("ray", "box"):
            baseline, upgraded = work / ("baseline-" + name), work / ("upgrade-" + name)
            previous = None
            for stage, action, edited in (("upgrade", "save", False), ("restart", "save", False),
                                          ("preferences", "preferences", True), ("preference-restart", "save", True)):
                checkpoint = f"{stage}.json"
                code = run_owned(staged_probe, ["-appdata", str(upgraded.resolve()), "-tray", "-many",
                                                "--package-action", action, "--package-report", checkpoint],
                                 payload, clean_environment(), work / f"{name}-{stage}.log",
                                 on_started=lambda: report.update(native_executed=True))
                if code:
                    raise RuntimeError(f"Package probe failed for {name}/{stage}: exit {code}")
                result = read_json(upgraded / checkpoint)
                if not result["action_completed"] or not result["normal_exit"] or result["production_exit_code"]:
                    raise AssertionError("Package probe did not complete a normal save and exit")
                assert_retained(baseline, upgraded, edited)
                current = stable_snapshot(upgraded)
                if stage in ("restart", "preference-restart") and current != previous:
                    raise AssertionError(f"Restart changed persisted configuration: {name}/{stage}")
                previous = current
                assert_original_bytes(baseline)
                report["checkpoints"].append({"core": name, "stage": stage, "result": "passed"})
            # Rollback restores the pre-upgrade backup into another fresh
            # directory; it never overwrites the upgraded or live installation.
            assert_original_bytes(work / ("rollback-" + name))
            report["checkpoints"].append({"core": name, "stage": "rollback-backup-byte-identity", "result": "passed"})
        report["result"] = "passed"
    except Exception as error:
        report.update(result="failed", failure=str(error))
        raise
    finally:
        write_json(work / "acceptance.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "static", "lifecycle"))
    parser.add_argument("--work", type=Path, help="A new disposable directory; never a user installation")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--probe", type=Path)
    arguments = parser.parse_args()
    work = arguments.work or Path(__file__).resolve().parents[2] / "build" / "package-acceptance" / uuid.uuid4().hex
    if arguments.mode == "prepare":
        result = prepare(work)
    elif arguments.archive is None:
        parser.error("--archive is required for static/lifecycle")
    elif arguments.mode == "static":
        work.mkdir(parents=True, exist_ok=False)
        result = verify_archive(arguments.archive.resolve(), work / "candidate")
        write_json(work / "static-verification.json", result)
    else:
        if arguments.probe is None:
            parser.error("--probe is required for lifecycle")
        result = lifecycle(work, arguments.archive.resolve(), arguments.probe.resolve())
    print(json.dumps({"work": str(work.resolve()), **result}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
