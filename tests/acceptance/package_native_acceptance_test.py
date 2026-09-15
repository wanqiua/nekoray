"""Pure orchestration tests: every process launch and GUI read is mocked."""

import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

import package_acceptance_test as archive_fixtures
import package_native_acceptance as native
from package_acceptance import verify_archive
from package_fixture import assert_original_bytes, create_fixture, digest, read_json, write_json
from package_windows_gui import RuntimeNotReady, verify_runtime_sources
from package_windows_process import OwnedProcess, StartupOnlyAcceptance


class NativePackageAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nekoray-native-package-mock-")
        self.root = Path(self.temporary.name)
        self.environment = {"SYSTEMROOT": str(self.root / "Windows"), "PATH": "system-only", "QT_QPA_PLATFORM": "windows"}
        self.powershell = self.file("tools/pwsh.exe")
        self.file("Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
        self.smoke = self.file("tools/nekoray_package_smoke.exe")
        self.gui_calls = []
        self.helper_calls = []
        self.groups = ["合成订阅组", "手动测试组"]
        self.normal_exit = True
        self.startup_only = False
        self.identity_verified = True
        self.accept_bad_tls = False
        self.fail_good_tls = False
        self.inspection = {"window": 42, "runtime_modules": [{"name": "Qt6Core.dll", "origin": "payload"}]}

    def tearDown(self):
        self.temporary.cleanup()

    def file(self, name, content=b"synthetic runtime"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def archive(self):
        fixture = archive_fixtures.PackageAcceptanceTests()
        fixture.root = self.root
        return fixture.archive()

    def gui_payload(self, name):
        payload = self.root / name
        for relative in ("nekoray.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll",
                         "Qt6Svg.dll", "platforms/qwindows.dll"):
            self.file(str(Path(name) / relative))
        return payload

    def verified_payload(self):
        report = verify_archive(self.archive(), self.root / "verified")
        path = self.root / "static-verification.json"
        write_json(path, report)
        return Path(report["payload"]), path

    def runner(self, executable, arguments, cwd, environment, log, **options):
        if options.get("on_started"):
            options["on_started"]()
        if executable.name == "nekoray.exe":
            self.gui_calls.append({"executable": executable, "arguments": arguments, **options})
            options["observe"](OwnedProcess(220, 99, 123456, time.monotonic() + 30))
            if self.startup_only:
                raise StartupOnlyAcceptance("Mocked owned-job cleanup completed")
            return 0
        self.helper_calls.append({"executable": executable, "arguments": arguments, **options})
        if "-TargetPid" in arguments:
            self.assertEqual(arguments[arguments.index("-TargetPid") + 1], "220")
            self.assertEqual(arguments[arguments.index("-Created") + 1], "123456")
            self.assertEqual(arguments[arguments.index("-Window") + 1], "42")
            write_json(Path(arguments[arguments.index("-Report") + 1]),
                       {"identity_verified": self.identity_verified, "groups_visible": self.groups,
                        "exit_invoked": self.normal_exit})
            return 0
        if "-Certificate" in arguments:
            Path(arguments[arguments.index("-Certificate") + 1]).write_text("mock certificate", encoding="utf-8")
            Path(arguments[arguments.index("-PrivateKey") + 1]).write_text("mock private key", encoding="utf-8")
            return 0
        if executable.name == "nekoray_package_smoke.exe":
            good = Path(arguments[0]).name == "trusted-localhost.pem"
            if (good and not self.fail_good_tls) or (not good and self.accept_bad_tls):
                log.write_text("HTTPS certificate verification and 204 response passed.\n", encoding="utf-8")
                return 0
            log.write_text("SSL handshake failed 0\n", encoding="utf-8")
            return 8
        raise AssertionError("Unexpected launch in a mock test")

    def run_gui(self, *, legacy=False):
        payload = self.gui_payload("gui")
        baseline = self.root / "baseline"
        create_fixture(baseline, 0)
        with patch.object(native, "run_owned", side_effect=self.runner), \
             patch.object(native, "inspect_owned_gui", return_value=self.inspection):
            return native.run_gui(payload, baseline, self.root, "case", self.environment, lambda: None,
                                  legacy=legacy, timeout=30)

    def test_verified_payload_is_rechecked_and_copied_to_a_new_tree(self):
        payload, report = self.verified_payload()
        result = native.stage_verified_payload(payload, report, self.root / "copy")
        self.assertTrue(result["copied_from_verified_payload"])
        self.assertEqual(digest(payload / "nekoray.exe"), digest(Path(result["payload"]) / "nekoray.exe"))
        self.assertNotEqual(Path(result["payload"]), payload)

    def test_verified_payload_tampering_is_rejected_before_copying(self):
        payload, report = self.verified_payload()
        (payload / "nekoray.exe").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed after verification"):
            native.stage_verified_payload(payload, report, self.root / "copy")
        self.assertFalse((self.root / "copy").exists())

    def test_verified_manifest_changes_are_rejected(self):
        payload, report = self.verified_payload()
        with (payload / "package-manifest.json").open("ab") as stream:
            stream.write(b" ")
        with self.assertRaisesRegex(ValueError, "manifest changed"):
            native.stage_verified_payload(payload, report, self.root / "copy")

    def test_prior_verification_must_be_for_exact_payload_and_current_manifest(self):
        payload, report_path = self.verified_payload()
        original = read_json(report_path)
        for replacement in ({"manifest_sha256": ""}, {"source_archive_verified": False}, {"go_build_info_verified": False},
                            {"go_module_notices_verified": False}, {"payload": str(self.root / "other")}):
            with self.subTest(replacement=replacement):
                write_json(report_path, {**original, **replacement})
                with self.assertRaisesRegex(ValueError, "current static verifier"):
                    native.stage_verified_payload(payload, report_path, self.root / "copy")

    def test_added_payload_file_is_rejected(self):
        payload, report = self.verified_payload()
        (payload / "unlisted.txt").write_text("not verified", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "extracted payload changed"):
            native.stage_verified_payload(payload, report, self.root / "copy")

    def test_legacy_copy_does_not_enter_configuration_or_logs(self):
        source = self.gui_payload("legacy")
        for name in ("config/groups/real.json", "subdir/config/private.json", "logs/session.txt", "notes.log"):
            self.file("legacy/" + name, b"never read")
        original_iterdir = Path.iterdir

        def inspect_directory(path):
            self.assertFalse(any(part.casefold() in native.OMIT_DIRECTORIES for part in path.relative_to(source).parts))
            return original_iterdir(path)

        with patch.object(Path, "iterdir", inspect_directory):
            result = native.stage_legacy_reference(source, self.root / "copy")
        self.assertFalse(result["source_configuration_read"])
        self.assertFalse(result["source_configuration_copied"])
        self.assertFalse((self.root / "copy/config").exists())
        self.assertFalse((self.root / "copy/subdir/config").exists())
        self.assertEqual((source / "config/groups/real.json").read_bytes(), b"never read")
        self.assertEqual(len(result["files"]), 7)

    def test_legacy_copy_rejects_destination_inside_source(self):
        source = self.gui_payload("legacy")
        with self.assertRaisesRegex(ValueError, "inside its source"):
            native.stage_legacy_reference(source, source / "nested")

    def test_native_environment_discards_developer_paths_proxy_and_core_overrides(self):
        variables = {"PATH": "developer-tools", "QT_PLUGIN_PATH": "developer-plugins", "QML2_IMPORT_PATH": "qml",
                     "HTTPS_PROXY": "http://proxy.invalid", "NEKORAY_FOO": "x", "NKR_FOO": "x", "XRAY_LOCATION_ASSET": "x",
                     "V2RAY_LOCATION_CONFIG": "x", "SING_BOX_TEST": "x", "SSL_CERT_FILE": "x", "SSL_CERT_DIR": "x",
                     "SystemRoot": "C:\\Windows", "KEEP": "yes"}
        with patch.dict(os.environ, variables, clear=True):
            environment = native.native_environment()
        self.assertEqual(environment["QT_QPA_PLATFORM"], "windows")
        self.assertEqual(environment["KEEP"], "yes")
        self.assertNotIn("developer", environment["PATH"])
        for key in variables.keys() - {"PATH", "SystemRoot", "KEEP"}:
            self.assertNotIn(key, environment)

    def test_native_fixture_only_exposes_the_copy_window(self):
        source = self.root / "baseline"
        create_fixture(source, 0)
        native.stage_native_fixture(source, self.root / "copy")
        assert_original_bytes(source)
        for name in ("nekoray.json", "nekobox.json"):
            self.assertTrue(read_json(source / "config/groups" / name)["start_minimal"])
            self.assertFalse(read_json(self.root / "copy/config/groups" / name)["start_minimal"])
            self.assertFalse(read_json(self.root / "copy/config/groups" / name)["remember_enable"])

    def test_unsafe_native_fixture_is_rejected_before_launch(self):
        source = self.root / "baseline"
        create_fixture(source, 0)
        relative = "groups/nekoray.json"
        settings = read_json(source / "config" / relative)
        settings["remember_enable"] = True
        write_json(source / "config" / relative, settings)
        marker = read_json(source / "package-fixture.json")
        marker["initial_files"][relative] = digest(source / "config" / relative)
        write_json(source / "package-fixture.json", marker)
        with self.assertRaisesRegex(ValueError, "enable a connection"):
            native.stage_native_fixture(source, self.root / "copy")

    def test_runtime_outside_candidate_is_rejected(self):
        payload = self.gui_payload("gui")
        outside = self.file("developer/Qt6Network.dll")
        with self.assertRaisesRegex(RuntimeError, "outside the isolated payload"):
            verify_runtime_sources([outside], payload, [])

    def test_candidate_cannot_use_installed_crt_but_legacy_reports_it(self):
        payload = self.gui_payload("gui")
        runtime = self.file("Windows/System32/msvcp140.dll")
        with self.assertRaisesRegex(RuntimeError, "outside the isolated payload"):
            verify_runtime_sources([runtime], payload, [])
        modules = verify_runtime_sources([runtime], payload, [], legacy_system_directory=runtime.parent)
        self.assertEqual(modules[0]["origin"], "system-crt")
        self.assertEqual(modules[0]["sha256"], digest(runtime))

    def test_missing_required_runtime_is_not_ready(self):
        payload = self.gui_payload("gui")
        with self.assertRaises(RuntimeNotReady):
            verify_runtime_sources([payload / "Qt6Core.dll"], payload, ["Qt6Core.dll", "qwindows.dll"])

    def test_candidate_observes_fixture_groups_and_exits_through_owned_menu(self):
        result = self.run_gui()
        self.assertEqual(result["result"], "passed")
        self.assertTrue(result["normal_exit"])
        self.assertTrue(result["synthetic_groups_visible"])
        self.assertTrue(self.gui_calls[0]["show_window"])
        self.assertFalse(self.gui_calls[0]["allow_startup_only"])
        assert_original_bytes(self.root / "baseline")

    def test_candidate_missing_group_fails_with_partial_report(self):
        self.groups = ["合成订阅组"]
        with self.assertRaisesRegex(RuntimeError, "synthetic fixture groups"):
            self.run_gui()
        result = read_json(self.root / "case-result.json")
        self.assertEqual(result["result"], "failed")
        self.assertTrue(result["window_ready"])
        self.assertFalse(result["synthetic_groups_visible"])

    def test_unverified_process_identity_prevents_acceptance(self):
        self.identity_verified = False
        with self.assertRaisesRegex(RuntimeError, "retained process identity"):
            self.run_gui(legacy=True)

    def test_legacy_startup_only_is_explicitly_weaker(self):
        self.normal_exit = False
        self.startup_only = True
        result = self.run_gui(legacy=True)
        self.assertEqual(result["result"], "startup-only")
        self.assertEqual(result["scope"], "startup-only")
        self.assertFalse(result["normal_exit"])
        self.assertEqual(result["cleanup"], "owned-job-confirmed")
        self.assertTrue(self.gui_calls[0]["allow_startup_only"])

    def test_candidate_never_accepts_startup_only(self):
        self.normal_exit = False
        self.startup_only = True
        with self.assertRaises(StartupOnlyAcceptance):
            self.run_gui()
        self.assertEqual(read_json(self.root / "case-result.json")["result"], "failed")

    def test_spontaneous_exit_does_not_count_as_normal_menu_exit(self):
        self.normal_exit = False
        with self.assertRaisesRegex(RuntimeError, "without completing"):
            self.run_gui()

    def run_tls(self):
        payload = self.gui_payload("gui")
        with patch.object(native, "run_owned", side_effect=self.runner):
            return native.run_tls(payload, self.smoke, self.root, self.environment, self.powershell, lambda: None)

    def test_tls_checks_success_and_hostname_failure(self):
        result = self.run_tls()
        self.assertEqual([case["result"] for case in result["cases"]], ["passed", "passed"])
        self.assertEqual(result["cases"][0]["http_status"], 204)
        self.assertTrue(result["cases"][1]["tls_handshake_rejected"])
        self.assertFalse(result["certificate_store_modified"])

    def test_tls_failure_with_trusted_hostname_is_rejected(self):
        self.fail_good_tls = True
        with self.assertRaisesRegex(RuntimeError, "did not verify"):
            self.run_tls()

    def test_tls_success_with_wrong_hostname_is_rejected(self):
        self.accept_bad_tls = True
        with self.assertRaisesRegex(RuntimeError, "did not reject"):
            self.run_tls()

    def test_missing_smoke_fails_before_any_native_process(self):
        work = self.root / "acceptance"
        with patch.object(native, "run_owned") as runner, \
             patch.object(native, "native_environment", return_value=self.environment):
            with self.assertRaisesRegex(ValueError, "Missing TLS smoke"):
                native.native_acceptance(work, archive=self.archive(), smoke=self.root / "missing.exe",
                                         legacy_source=self.gui_payload("legacy"), powershell=self.powershell)
        runner.assert_not_called()
        result = read_json(work / "native-acceptance.json")
        self.assertEqual(result["result"], "failed")
        self.assertFalse(result["native_executed"])

    def test_missing_legacy_runtime_fails_before_any_native_process(self):
        legacy = self.gui_payload("legacy")
        (legacy / "Qt6Network.dll").unlink()
        with patch.object(native, "run_owned") as runner, \
             patch.object(native, "native_environment", return_value=self.environment):
            with self.assertRaisesRegex(ValueError, "missing runtime modules"):
                native.native_acceptance(self.root / "acceptance", archive=self.archive(), smoke=self.smoke,
                                         legacy_source=legacy, powershell=self.powershell)
        runner.assert_not_called()

    def test_complete_native_orchestration_is_fully_mocked(self):
        with patch.object(native, "run_owned", side_effect=self.runner), \
             patch.object(native, "inspect_owned_gui", return_value=self.inspection), \
             patch.object(native, "native_environment", return_value=self.environment):
            result = native.native_acceptance(self.root / "acceptance", archive=self.archive(), smoke=self.smoke,
                                              legacy_source=self.gui_payload("legacy"), powershell=self.powershell)
        self.assertEqual(result["result"], "passed")
        self.assertTrue(result["native_executed"])
        self.assertFalse(result["system_modes_enabled"])
        self.assertTrue(result["legacy_normal_exit_verified"])
        self.assertEqual(len(result["checkpoints"]), 4)
        self.assertEqual(len(self.gui_calls), 4)
        self.assertEqual(len(self.helper_calls), 8)
        self.assertEqual([case["allow_startup_only"] for case in self.gui_calls], [False, False, True, True])


if __name__ == "__main__":
    unittest.main()
