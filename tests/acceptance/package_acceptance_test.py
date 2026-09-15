import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from package_acceptance import PRODUCT_NAME, GUI_FILENAME, REQUIRED_FILES, REQUIRED_SOURCE_FILES, lifecycle, verify_archive, verify_source_archive
from package_fixture import (assert_original_bytes, assert_retained, copy_fixture, create_fixture,
                             read_json, require_fixture, write_json)
from package_go_build_info_test import synthetic_go_payload


class PackageAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nekoray-package-self-test-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def archive(self, edit=None, extra_entries=(), omit_source=(), branded=False):
        payload = {name: ("synthetic " + name).encode() for name in REQUIRED_FILES}
        root = PRODUCT_NAME if branded else "nekoray"
        branding = {"product": PRODUCT_NAME, "gui_version": "0.1"} if branded else {}
        if branded:
            payload[GUI_FILENAME] = payload.pop("nekoray.exe")
        go_payload, cores = synthetic_go_payload()
        payload.update(go_payload)
        payload["core-versions.json"] = json.dumps({"gui_base": "3.26", "target_os": "windows", "target_arch": "amd64", "cores": cores, **branding}).encode()
        source = self.root / "fixture-source.zip"
        with zipfile.ZipFile(source, "w") as zipped:
            for name in REQUIRED_SOURCE_FILES - {"BUILD-SOURCE.json"} - set(omit_source):
                zipped.writestr(root + "/" + name, "Synthetic source archive")
            zipped.writestr(root + "/BUILD-SOURCE.json", json.dumps({"base_revision": "1" * 40, "includes_working_tree_changes": True}))
        manifest = {"gui_base": "3.26", "cores": copy.deepcopy(cores),
                    "source": {"archive": source.name, "sha256": hashlib.sha256(source.read_bytes()).hexdigest()},
                    "files": [{"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}
                              for name, content in payload.items()]}
        manifest.update(branding)
        if branded:
            manifest["gui_executable"] = GUI_FILENAME
        if edit:
            edit(payload, manifest)
        archive = self.root / "candidate.zip"
        with zipfile.ZipFile(archive, "w") as zipped:
            for name, content in payload.items():
                zipped.writestr(root + "/" + name, content)
            zipped.writestr(root + "/package-manifest.json", json.dumps(manifest))
            for name, content in extra_entries:
                zipped.writestr(name, content)
        return archive

    def rejected(self, archive):
        target = self.root / "extracted"
        with self.assertRaises((ValueError, FileNotFoundError)):
            verify_archive(archive, target)
        self.assertFalse(target.exists(), "Invalid archives must be rejected before extraction")

    def test_valid_archive_checks_complete_inventory_and_source(self):
        report = verify_archive(self.archive(), self.root / "extracted")
        self.assertEqual(report["files_verified"], len(REQUIRED_FILES) + 7)
        self.assertTrue(report["source_archive_verified"])
        self.assertTrue(report["go_build_info_verified"])
        self.assertTrue(report["go_module_notices_verified"])
        self.assertEqual(report["go_modules_verified"], 6)
        self.assertFalse(report["native_executed"])

    def test_traversal_archive_is_rejected_before_extraction(self):
        self.rejected(self.archive(extra_entries=(("nekoray/../../outside.txt", b"outside"),)))
        self.assertFalse((self.root / "outside.txt").exists())

    def test_branded_archive_keeps_the_new_executable_and_root(self):
        report = verify_archive(self.archive(branded=True), self.root / "extracted")
        payload = Path(report["payload"])
        self.assertEqual(payload.name, PRODUCT_NAME)
        self.assertTrue((payload / GUI_FILENAME).is_file())
        self.assertFalse((payload / "nekoray.exe").exists())

    def test_branded_archive_rejects_mixed_core_versions(self):
        self.rejected(self.archive(lambda payload, manifest: manifest.update(gui_version="0.2"), branded=True))

    def test_branded_archive_requires_certificate_pin_sources(self):
        self.rejected(self.archive(omit_source=("go/cmd/nekobox_core/hysteria2_pin.go",), branded=True))

    def test_duplicate_manifest_record_is_rejected(self):
        self.rejected(self.archive(lambda payload, manifest: manifest["files"].append(manifest["files"][0])))

    def test_case_colliding_archive_entries_are_rejected(self):
        self.rejected(self.archive(extra_entries=(("nekoray/qt6core.dll", b"collision"),)))

    def test_unlisted_archive_member_is_rejected(self):
        self.rejected(self.archive(extra_entries=(("nekoray/unlisted.txt", b"unlisted"),)))

    def test_manifest_cannot_escape_the_payload(self):
        self.rejected(self.archive(lambda payload, manifest: manifest["files"][0].update(path="../outside")))

    def test_content_hash_mismatch_is_rejected(self):
        self.rejected(self.archive(lambda payload, manifest: payload.update({"nekoray.exe": b"x" * len(payload["nekoray.exe"])})))

    def test_incompatible_xray_is_rejected(self):
        self.rejected(self.archive(lambda payload, manifest: manifest["cores"]["nekoray_core"].update(version="v26.9.1")))

    def test_user_config_must_not_be_in_a_portable(self):
        def edit(payload, manifest):
            content = b"synthetic configuration"
            name = "config/groups/nekoray.json"
            payload[name] = content
            manifest["files"].append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
        self.rejected(self.archive(edit))

    def test_source_archive_must_match(self):
        archive = self.archive()
        (self.root / "fixture-source.zip").write_bytes(b"changed")
        self.rejected(archive)

    def test_new_protocol_implementation_and_local_dependency_source_are_required(self):
        for name in ("fmt/AnyTLSBean.cpp", "fmt/SnellBean.hpp", "fmt/QUICBean.hpp", "ui/edit/edit_quic.ui",
                     "fmt/Link2Bean.cpp", "tests/new_protocol_regression_test.hpp", "go/grpc_server/go.mod",
                     "go/grpc_server/probe/url.go", "libs/prepare_package_notices.ps1",
                     "tests/acceptance/package_go_build_info.py"):
            with self.subTest(name=name):
                self.rejected(self.archive(omit_source=(name,)))

    @staticmethod
    def replace_payload_file(payload, manifest, name, content):
        payload[name] = content
        for entry in manifest["files"]:
            if entry["path"] == name:
                entry.update(bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
                return
        raise AssertionError("A mutation must name an existing fixture file")

    @staticmethod
    def remove_payload_file(payload, manifest, name):
        del payload[name]
        manifest["files"] = [entry for entry in manifest["files"] if entry["path"] != name]

    def test_missing_build_info_and_module_inventory_are_rejected_before_extraction(self):
        for name in ("licenses/nekoray_core-build-info.txt", "licenses/nekobox_core-build-info.txt", "licenses/go-modules.json"):
            with self.subTest(name=name):
                self.rejected(self.archive(lambda payload, manifest: self.remove_payload_file(payload, manifest, name)))

    def test_stale_build_info_with_valid_manifest_hash_is_rejected_before_extraction(self):
        def edit(payload, manifest):
            name = "licenses/nekobox_core-build-info.txt"
            self.replace_payload_file(payload, manifest, name, payload[name].replace(b"go1.26.4", b"go1.26.3"))
        self.rejected(self.archive(edit))

    def test_incomplete_module_inventory_with_valid_hash_is_rejected_before_extraction(self):
        def edit(payload, manifest):
            name = "licenses/go-modules.json"
            rows = json.loads(payload[name])
            rows.pop()
            self.replace_payload_file(payload, manifest, name, json.dumps(rows).encode())
        self.rejected(self.archive(edit))

    def test_missing_module_notice_is_rejected_before_extraction(self):
        def edit(payload, manifest):
            name = next(path for path in payload if path.startswith("licenses/go/") and path.endswith("/LICENSE"))
            self.remove_payload_file(payload, manifest, name)
        self.rejected(self.archive(edit))

    def test_malformed_embedded_build_info_is_rejected_even_when_all_hashes_agree(self):
        def edit(payload, manifest):
            name = "nekobox_core.exe"
            changed = payload[name].replace(b"\xff Go buildinf:", b"x Go buildinf:")
            self.replace_payload_file(payload, manifest, name, changed)
            core_manifest = json.loads(payload["core-versions.json"])
            core_manifest["cores"]["nekobox_core"]["sha256"] = hashlib.sha256(changed).hexdigest()
            manifest["cores"] = copy.deepcopy(core_manifest["cores"])
            self.replace_payload_file(payload, manifest, "core-versions.json", json.dumps(core_manifest).encode())
        self.rejected(self.archive(edit))

    def test_source_archive_rejects_runtime_snapshots_and_caches(self):
        for relative in ("build/system/state.dpapi", "build64/system/config-hashes.json", "build_debug/system/state.json",
                         "deployment/config/profiles/1.json", "tests/acceptance/__pycache__/note.txt"):
            self.archive()
            source = self.root / "fixture-source.zip"
            with zipfile.ZipFile(source, "a") as zipped:
                zipped.writestr("nekoray/" + relative, "Synthetic forbidden artifact")
            with self.assertRaisesRegex(ValueError, "runtime, configuration or cache"):
                verify_source_archive(source)

    def test_archive_failure_writes_lifecycle_report_without_running_a_process(self):
        archive = self.archive(extra_entries=(("nekoray/../../outside.txt", b"outside"),))
        work = self.root / "lifecycle"
        with patch("package_windows_process.run_owned") as runner:
            with self.assertRaises(ValueError):
                lifecycle(work, archive, self.root / "missing-probe.exe")
            runner.assert_not_called()
        report = read_json(work / "acceptance.json")
        self.assertEqual(report["result"], "failed")
        self.assertFalse(report["native_executed"])
        self.assertFalse((work / "candidate").exists())

    def test_probe_copy_failure_writes_lifecycle_report_without_running_a_process(self):
        work = self.root / "lifecycle"
        with patch("package_windows_process.run_owned") as runner:
            with self.assertRaises(FileNotFoundError):
                lifecycle(work, self.archive(), self.root / "missing-probe.exe")
            runner.assert_not_called()
        report = read_json(work / "acceptance.json")
        self.assertEqual(report["result"], "failed")
        self.assertFalse(report["native_executed"])
        self.assertTrue(report["archive"]["source_archive_verified"])

    def migrated_fixture(self, core=0):
        original, upgraded = self.root / "baseline", self.root / "upgrade"
        create_fixture(original, core)
        copy_fixture(original, upgraded)
        for path in (upgraded / "config/profiles").glob("*.json"):
            node = read_json(path)
            legacy = {"status": "legacy", "core": -1, "scope": "legacy", "latency_ms": node["yc"], "error": node["report"]}
            node.update(yc=0, report="", core_preference=0, test_results=json.dumps({"legacy_migrated": True, "history": [legacy]}))
            write_json(path, node)
        return original, upgraded

    def test_synthetic_backup_and_rollback_keep_original_bytes(self):
        original, upgraded = self.migrated_fixture()
        rollback = self.root / "rollback"
        copy_fixture(original, rollback)
        assert_original_bytes(original)
        assert_original_bytes(rollback)
        assert_retained(original, upgraded)

    def test_fixture_matches_legacy_empty_string_and_authorization_serialization(self):
        original = self.root / "baseline"
        create_fixture(original, 0)
        self.assertNotIn("url", read_json(original / "config/groups/3.json"))
        self.assertEqual(read_json(original / "config/groups/17.json")["url"], "https://subscription.invalid/fixture")
        settings = read_json(original / "config/groups/nekoray.json")
        self.assertEqual(settings["inbound_auth"], {"user": "fixture-local", "pass": "synthetic-only"})
        self.assertNotIn("hk_mw", settings)
        self.assertEqual(settings["spmode2"], [])

    def test_native_serialization_can_omit_cleared_legacy_report(self):
        original, upgraded = self.migrated_fixture()
        for path in (upgraded / "config/profiles").glob("*.json"):
            node = read_json(path)
            del node["report"]
            write_json(path, node)
        assert_retained(original, upgraded)

    def test_nonempty_authorization_loss_is_not_an_empty_string_normalization(self):
        original, upgraded = self.migrated_fixture()
        path = upgraded / "config/groups/nekoray.json"
        settings = read_json(path)
        del settings["inbound_auth"]["pass"]
        write_json(path, settings)
        with self.assertRaisesRegex(AssertionError, "inbound_auth/pass"):
            assert_retained(original, upgraded)

    def test_unmarked_directory_is_never_a_fixture(self):
        with self.assertRaisesRegex(ValueError, "marker"):
            require_fixture(self.root)

    def test_insecure_and_rc4_md5_values_are_checked(self):
        original, upgraded = self.migrated_fixture()
        path = upgraded / "config/profiles/47.json"
        node = read_json(path)
        node["bean"]["method"] = "aes-128-gcm"
        write_json(path, node)
        with self.assertRaisesRegex(AssertionError, "bean/method"):
            assert_retained(original, upgraded)
        node["bean"]["method"] = "rc4-md5"
        write_json(path, node)
        path = upgraded / "config/profiles/83.json"
        node = read_json(path)
        node["bean"]["stream"]["insecure"] = False
        write_json(path, node)
        with self.assertRaisesRegex(AssertionError, "stream/insecure"):
            assert_retained(original, upgraded)

    def test_subscription_order_is_checked(self):
        original, upgraded = self.migrated_fixture()
        path = upgraded / "config/groups/17.json"
        group = read_json(path)
        group["order"].reverse()
        write_json(path, group)
        with self.assertRaisesRegex(AssertionError, "order"):
            assert_retained(original, upgraded)

    def test_repeated_legacy_migration_is_rejected(self):
        original, upgraded = self.migrated_fixture()
        path = upgraded / "config/profiles/11.json"
        node = read_json(path)
        results = json.loads(node["test_results"])
        results["history"] *= 2
        node["test_results"] = json.dumps(results)
        write_json(path, node)
        with self.assertRaisesRegex(AssertionError, "duplicated"):
            assert_retained(original, upgraded)

    def test_manual_preferences_and_inactive_settings_are_checked(self):
        original, upgraded = self.migrated_fixture(core=1)
        marker = read_json(original / "package-fixture.json")
        for key, preference in marker["node_preferences_after_edit"].items():
            path = upgraded / f"config/profiles/{key}.json"
            node = read_json(path)
            node["core_preference"] = preference
            write_json(path, node)
        path = upgraded / "config/groups/nekobox.json"
        settings = read_json(path)
        settings["preferred_core"] = 0
        write_json(path, settings)
        assert_retained(original, upgraded, preferences_edited=True)
        inactive = upgraded / "config/groups/nekoray.json"
        inactive.write_text(json.dumps(read_json(inactive)), encoding="utf-8")
        with self.assertRaisesRegex(AssertionError, "Unselected"):
            assert_retained(original, upgraded, preferences_edited=True)

    @unittest.skipUnless(os.name == "nt", "Windows process ownership")
    def test_owned_process_completes_without_children(self):
        from package_windows_process import run_owned
        started = []
        code = run_owned(Path(sys.executable), ["-c", "print('owned fixture')"], self.root,
                         dict(os.environ), self.root / "process.log", timeout=5, on_started=lambda: started.append(True))
        self.assertEqual(code, 0)
        self.assertEqual(started, [True])

    @unittest.skipUnless(os.name == "nt", "Windows process ownership")
    def test_owned_timeout_stops_only_its_job(self):
        from package_windows_process import run_owned
        with self.assertRaises(TimeoutError):
            run_owned(Path(sys.executable), ["-c", "import time; time.sleep(5)"], self.root,
                      dict(os.environ), self.root / "timeout.log", timeout=1)

    @unittest.skipUnless(os.name == "nt", "Windows process ownership")
    def test_leaked_owned_child_is_a_failure(self):
        from package_windows_process import run_owned
        code = "import subprocess,sys; subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])"
        with self.assertRaisesRegex(RuntimeError, "child"):
            run_owned(Path(sys.executable), ["-c", code], self.root, dict(os.environ), self.root / "leak.log", timeout=5)


if __name__ == "__main__":
    unittest.main()
