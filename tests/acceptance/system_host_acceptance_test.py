"""Pure fault-injection tests for the host runner; never alters host settings."""

import copy
from contextlib import ExitStack
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import system_host_acceptance as runner


class FakeProcesses:
    def __init__(self, tables):
        self.tables = iter(tables)

    def process_table(self):
        return next(self.tables)


class FakeProxy:
    def __init__(self, states):
        self.states = copy.deepcopy(states)
        self.writes = []
        self.refreshes = 0

    def connections(self):
        return list(self.states)

    def proxy_read(self, connection):
        return dict(self.states[connection])

    def proxy_write(self, connection, state):
        self.writes.append(connection)
        self.states[connection] = dict(state)

    def refresh_proxy(self):
        self.refreshes += 1


class FakeManagedProcess:
    def __init__(self, pid, events):
        self.identity = {"pid": pid, "created": pid, "image": "synthetic.exe"}
        self.suspended = False
        self.events = events

    def alive(self):
        return True

    def suspend(self):
        self.suspended = True

    def resume(self):
        self.events.append("resume")
        self.suspended = False

    def close(self):
        pass


class HostAcceptanceTest(unittest.TestCase):
    def test_inaccessible_core_is_not_reported_as_exited(self):
        table = {42: {"pid": 42, "ppid": 1, "name": "nekobox_core.exe"}}
        native = FakeProcesses([table, table])
        with patch.object(runner, "Process", side_effect=runner.AcceptanceError("synthetic access denied")):
            with self.assertRaisesRegex(runner.AcceptanceError, "cleanup is not confirmed"):
                runner.test_core_identities(native, Path("synthetic-build"))

    def test_exited_core_can_disappear_during_identity_read(self):
        table = {42: {"pid": 42, "ppid": 1, "name": "nekobox_core.exe"}}
        native = FakeProcesses([table, {}])
        with patch.object(runner, "Process", side_effect=runner.AcceptanceError("synthetic process exited")):
            self.assertEqual(runner.test_core_identities(native, Path("synthetic-build")), {})

    def test_both_system_cases_must_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "qtest.xml"
            report.write_text('<testsuite><testcase name="systemProxyRuntimeAcceptance"/>'
                              '<testcase name="tunSystemRuntimeAcceptance"/></testsuite>', encoding="utf-8")
            self.assertEqual(len(runner.completed_test_cases(report)), 2)
            report.write_text('<testsuite><testcase name="systemProxyRuntimeAcceptance"/></testsuite>', encoding="utf-8")
            with self.assertRaises(runner.AcceptanceError):
                runner.completed_test_cases(report)

    def test_skipped_or_failed_cases_are_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "qtest.xml"
            for tag in ("skipped", "failure", "error"):
                report.write_text('<testsuite><testcase name="systemProxyRuntimeAcceptance"/>'
                                  f'<testcase name="tunSystemRuntimeAcceptance"><{tag}/></testcase></testsuite>', encoding="utf-8")
                with self.subTest(tag=tag), self.assertRaises(runner.AcceptanceError):
                    runner.completed_test_cases(report)

    def test_only_the_own_audited_proxy_state_is_restored(self):
        saved = {"flags": 13, "server": "synthetic-prior:8080", "bypass": "<local>",
                 "pac": "https://example.test/proxy.pac"}
        applied = dict(saved, flags=3, server="127.0.0.1:12345")
        later = dict(applied, bypass="changed-by-another-app")
        native = FakeProxy({"": applied, "third-party": later})
        before = {"connections": {"": saved, "third-party": saved}, "registry": {}}
        with tempfile.TemporaryDirectory() as directory:
            audit = Path(directory) / "audit.json"
            audit.write_text(json.dumps({"servers": ["127.0.0.1:12345"]}), encoding="utf-8")
            with patch.object(runner, "registry_proxy", return_value={}):
                result = runner.restore_proxy(native, before, audit)
        self.assertEqual(native.writes, [""])
        self.assertEqual(native.states[""], saved)
        self.assertEqual(native.states["third-party"], later)
        self.assertFalse(result["semantic_state_restored"])
        self.assertEqual(result["connections_restored_by_guardian"], 1)

    def test_audit_does_not_authorize_non_loopback_proxy(self):
        with tempfile.TemporaryDirectory() as directory:
            audit = Path(directory) / "audit.json"
            audit.write_text(json.dumps({"servers": ["https=proxy.example.test:443"]}), encoding="utf-8")
            with self.assertRaisesRegex(runner.AcceptanceError, "non-loopback"):
                runner.safe_proxy_servers(audit)

    def test_native_dns_flush_checks_the_api_result(self):
        native = runner.Native.__new__(runner.Native)
        native.dns = SimpleNamespace(DnsFlushResolverCache=Mock(return_value=1))
        native.flush_dns_cache()
        native.dns.DnsFlushResolverCache.assert_called_once_with()
        native.dns.DnsFlushResolverCache.return_value = 0
        with self.assertRaisesRegex(runner.AcceptanceError, "DNS resolver cache cleanup returned failure"):
            native.flush_dns_cache()

    def _guarded_dns_case(self, scenario):
        events = []
        native = Mock()
        native.protect.return_value = b"synthetic encrypted backup"
        native.identity.return_value = {"pid": 80, "created": 80, "image": "synthetic-test.exe"}

        def flush():
            events.append("flush")
            if scenario == "flush_failure":
                raise runner.AcceptanceError("Synthetic DNS cache cleanup failed")

        native.flush_dns_cache.side_effect = flush
        original = FakeManagedProcess(20, events)
        refreshed = FakeManagedProcess(20, events)
        outer = FakeManagedProcess(10, events)
        child = Mock(pid=80, _handle=800, returncode=2 if scenario == "child_failure" else 0)
        child.poll.return_value = child.returncode
        if scenario == "after_launch_error":
            native.identity.side_effect = runner.AcceptanceError("Synthetic error after child launch")
        core_checks = 0

        def cores(*_):
            nonlocal core_checks
            core_checks += 1
            if core_checks > 1:
                if scenario == "core_cleanup_unknown":
                    raise runner.AcceptanceError("Synthetic core cleanup is not confirmed")
                events.append("cores_exited")
            return {}

        with tempfile.TemporaryDirectory() as directory, ExitStack() as mocks:
            output = Path(directory)
            configuration = {"gui_tests": str(output / "gui" / "gui-tests.exe"),
                             "installation": str(output / "installation"), "output": str(output),
                             "timeout": 30, "runner": outer.identity}
            for name, value in {
                "Native": Mock(return_value=native),
                "Process": Mock(return_value=outer),
                "validate_paths": Mock(),
                "installation_processes": Mock(side_effect=[([original], 1), ([refreshed], 1)]),
                "proxy_snapshot": Mock(return_value={"connections": {}, "registry": {}}),
                "network_snapshot": Mock(return_value={"routes": [], "dns": [], "adapters": []}),
                "config_hashes": Mock(return_value={}),
                "summarize": Mock(return_value={}),
                "test_core_identities": Mock(side_effect=cores),
                "completed_test_cases": Mock(return_value=["systemProxyRuntimeAcceptance", "tunSystemRuntimeAcceptance"]),
                "restore_proxy": Mock(return_value={"semantic_state_restored": True, "registry_restored": True}),
            }.items():
                mocks.enter_context(patch.object(runner, name, value))
            popen = mocks.enter_context(patch.object(runner.subprocess, "Popen", return_value=child))
            if scenario == "not_started":
                popen.side_effect = runner.AcceptanceError("Synthetic test launch failed")
            exit_code = runner.guarded_run(configuration)
            report = json.loads((output / "result.json").read_text(encoding="utf-8"))
        return exit_code, report, native, events

    def test_guardian_flushes_after_success_failure_and_exception(self):
        for scenario in ("success", "child_failure", "after_launch_error"):
            with self.subTest(scenario=scenario):
                exit_code, report, native, events = self._guarded_dns_case(scenario)
                self.assertEqual(exit_code, 0 if scenario == "success" else 1)
                self.assertEqual(report["dns_cache_cleanup"], {"attempted": True, "succeeded": True})
                native.flush_dns_cache.assert_called_once_with()
                self.assertLess(events.index("cores_exited"), events.index("flush"))
                self.assertLess(events.index("flush"), events.index("resume"))
                self.assertTrue(report["original_processes_resumed"])

    def test_dns_failure_is_recorded_and_original_processes_still_resume(self):
        exit_code, report, native, events = self._guarded_dns_case("flush_failure")
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["dns_cache_cleanup"]["attempted"], True)
        self.assertEqual(report["dns_cache_cleanup"]["succeeded"], False)
        self.assertIn("Synthetic DNS cache cleanup failed", report["failures"])
        native.flush_dns_cache.assert_called_once_with()
        self.assertTrue(report["original_processes_resumed"])
        self.assertTrue(report["proxy_recovery"]["semantic_state_restored"])

    def test_dns_cleanup_waits_for_a_started_test_and_confirmed_core_exit(self):
        for scenario, reason in (("not_started", "test_not_started"),
                                 ("core_cleanup_unknown", "test_core_cleanup_not_confirmed")):
            with self.subTest(scenario=scenario):
                exit_code, report, native, _ = self._guarded_dns_case(scenario)
                self.assertEqual(exit_code, 1)
                self.assertEqual(report["dns_cache_cleanup"], {"attempted": False, "succeeded": None, "reason": reason})
                native.flush_dns_cache.assert_not_called()
                self.assertTrue(report["original_processes_resumed"])

    def test_preflight_never_flushes_dns_cache_or_starts_a_guardian(self):
        native = Mock()
        with tempfile.TemporaryDirectory() as directory, ExitStack() as mocks:
            arguments = ["system_host_acceptance.py", "--gui-tests", str(Path(directory) / "gui-tests.exe"),
                         "--existing-install", str(Path(directory) / "installation"),
                         "--output-dir", str(Path(directory) / "reports")]
            mocks.enter_context(patch.object(runner.sys, "argv", arguments))
            # Patch only this module's OS facade. Native APIs, host inspection,
            # process creation and ACL writes are all replaced with mocks.
            mocks.enter_context(patch.object(runner, "os", SimpleNamespace(name="nt", replace=runner.os.replace)))
            mocks.enter_context(patch.object(runner, "Native", return_value=native))
            mocks.enter_context(patch.object(runner, "validate_paths"))
            mocks.enter_context(patch.object(runner, "inspect_host", return_value={}))
            mocks.enter_context(patch("builtins.print"))
            guarded = mocks.enter_context(patch.object(runner, "guarded_run"))
            popen = mocks.enter_context(patch.object(runner.subprocess, "Popen"))
            self.assertEqual(runner.main(), 0)
            native.flush_dns_cache.assert_not_called()
            guarded.assert_not_called()
            popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
