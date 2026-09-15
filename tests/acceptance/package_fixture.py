"""Synthetic 3.26 configuration fixtures; never accepts an unmarked user directory."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import uuid

MARKER = "package-fixture.json"
KIND = "nekoray-package-synthetic-config"


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def legacy_serialized(value):
    """Match 3.26 JsonStore: empty strings are omitted, arrays/objects are kept."""
    if isinstance(value, dict):
        return {key: legacy_serialized(item) for key, item in value.items() if item != ""}
    if isinstance(value, list):
        return [legacy_serialized(item) for item in value]
    return value


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require_plain_tree(root: Path):
    pending = [root]
    while pending:
        path = pending.pop()
        stat = path.lstat()
        if path.is_symlink() or getattr(stat, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Links are not accepted in a fixture: {path.name}")
        if path.is_dir():
            pending.extend(path.iterdir())


def require_fixture(root: Path) -> dict:
    if not root.is_dir():
        raise ValueError("Missing synthetic fixture directory")
    if not (root / MARKER).is_file():
        raise ValueError("This directory has no synthetic fixture marker")
    for path in (root, root / MARKER):
        if path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("Fixture paths must not be links")
    marker = read_json(root / MARKER)
    if marker.get("kind") != KIND or marker.get("schema") != 1 or marker.get("source_core") not in (0, 1):
        raise ValueError("This directory is not a generated package fixture")
    uuid.UUID(marker["run_id"])
    for relative in marker["initial_files"]:
        if not relative or "\\" in relative or ":" in relative or any(part in ("", ".", "..") for part in relative.split("/")):
            raise ValueError("Invalid fixture file path")
    require_plain_tree(root)
    return marker


def create_fixture(root: Path, core: int) -> dict:
    if core not in (0, 1) or root.exists():
        raise ValueError("Use a new directory and core 0 or 1")
    root.mkdir(parents=True)
    config = root / "config"
    (config / "groups").mkdir(parents=True)
    (config / "groups/coreType").write_text(str(core), encoding="utf-8")
    write_json(config / "groups/pm.json", {"groups": [17, 3]})
    for gid, name, order, url in (
        (3, "手动测试组", [29, 11], ""),
        (17, "合成订阅组", [83, 47], "https://subscription.invalid/fixture"),
    ):
        write_json(config / f"groups/{gid}.json", {
            "id": gid, "name": name, "order": order, "url": url,
            "archive": False, "skip_auto_update": True, "front_proxy_id": -1,
            "info": "Synthetic package acceptance only", "lastup": 1700000000,
            "manually_column_width": True, "column_width": [110, 220, 180, 120, 100],
        })
    nodes = (
        (11, 3, "http", {"v": -80, "username": "fixture", "password": "synthetic-only"}, 81),
        (29, 3, "socks", {"v": 5, "username": "", "password": ""}, -1),
        (47, 17, "shadowsocks", {"method": "rc4-md5", "pass": "synthetic-only", "plugin": "", "uot": 0}, 0),
        (83, 17, "vless", {"pass": "91c19358-b5da-49d6-a1d3-cc47973baed0", "flow": ""}, 42),
    )
    for node_id, gid, protocol, fields, latency in nodes:
        stream = {"net": "tcp", "sec": "", "insecure": False}
        if protocol == "vless":
            stream.update(net="ws", sec="tls", insecure=True, path="/fixture", host="node.invalid", sni="node.invalid")
        bean = {"_v": 0, "name": "同名测试节点" if node_id in (11, 29) else protocol,
                "addr": "127.0.0.1", "port": 9, "c_cfg": "", "c_out": "", "stream": stream, **fields}
        write_json(config / f"profiles/{node_id}.json", {
            "id": node_id, "gid": gid, "type": protocol, "bean": bean,
            "traffic": {"dl": 4294967296 + node_id, "ul": 2147483648 + node_id},
            "yc": latency, "report": f"Legacy synthetic report {node_id}",
        })
    for source, filename in ((0, "nekoray.json"), (1, "nekobox.json")):
        write_json(config / f"groups/{filename}", {
            "language": 1, "current_group": 17, "theme": "0", "start_minimal": True,
            "remember_enable": False, "remember_id": -1919, "spmode2": [],
            "sub_auto_update": -30, "sub_use_proxy": False, "sub_clear": True, "sub_insecure": False,
            "user_agent2": "Synthetic-Package-Acceptance/1", "active_routing": "Fixture",
            "inbound_address": "127.0.0.1", "inbound_socks_port": 29880, "inbound_http_port": 29881,
            "inbound_auth": {"user": "fixture-local", "pass": "synthetic-only"},
            "test_concurrent": 7, "test_latency_timeout": 9000, "test_url": "http://127.0.0.1:9/fixture",
            "test_url_dl": "http://127.0.0.1:9/download", "test_dl_timeout": 11,
            "log_level": "warning" if source == 0 else "warn", "mux_protocol": "smux", "mux_concurrency": 8,
            "mux_default_on": False, "mux_padding": False, "skip_cert": False, "utlsFingerprint": "",
            "extraCore": {"core_map": "{\n  \"unused-fixture-helper\": \"\"\n}"},
            "custom_inbound": "{\"inbounds\":[]}", "custom_route": "{\"rules\":[]}",
            "max_log_line": 333, "traffic_loop_interval": 0, "conn_stat": False, "check_include_pre": False,
            "hk_mw": "", "hk_group": "", "hk_route": "", "hk_spmenu": "",
        })
    for route_dir in ("routes", "routes_box"):
        write_json(config / route_dir / "Fixture", {
            "direct_domain": "full:localhost", "proxy_domain": "domain:example.invalid",
            "block_domain": "domain:blocked.invalid", "def_outbound": "proxy",
            "remote_dns": "1.1.1.1", "direct_dns": "localhost", "sniffing_mode": 0,
            "domain_strategy": "AsIs", "custom": "{\"rules\":[]}",
        })
    (config / "fixture-note.txt").write_text("Synthetic retained auxiliary file.\n", encoding="utf-8")
    # These are pre-upgrade on-disk files, not an arbitrary hand-written JSON
    # superset. Preserve the original serializer's omission of empty strings.
    for path in config.rglob("*"):
        if path.is_file() and (path.suffix == ".json" or path.parent.name in ("routes", "routes_box")):
            write_json(path, legacy_serialized(read_json(path)))
    hashes = {path.relative_to(config).as_posix(): digest(path) for path in sorted(config.rglob("*")) if path.is_file()}
    marker = {"schema": 1, "kind": KIND, "run_id": str(uuid.uuid4()), "source_core": core,
              "default_preference_after_edit": 1 - core,
              "node_preferences_after_edit": {"11": 1, "29": 2, "47": 1, "83": 2},
              "initial_files": hashes}
    write_json(root / MARKER, marker)
    return marker


def copy_fixture(source: Path, destination: Path):
    require_fixture(source)
    if destination.exists():
        raise ValueError("A fixture copy must use a new directory")
    shutil.copytree(source, destination)
    require_fixture(destination)


def assert_original_bytes(root: Path):
    marker = require_fixture(root)
    for relative, expected in marker["initial_files"].items():
        if digest(root / "config" / relative) != expected:
            raise AssertionError(f"Rollback differs from the original fixture: {relative}")


def assert_subset(expected, actual, location: str):
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            raise AssertionError(f"Object was lost: {location}")
        for key, value in expected.items():
            if key not in actual:
                raise AssertionError(f"Field was lost: {location}/{key}")
            assert_subset(value, actual[key], f"{location}/{key}")
    elif expected != actual:
        # Reports identify the changed field without printing node credentials.
        raise AssertionError(f"Field changed: {location}")


def assert_retained(original: Path, upgraded: Path, preferences_edited: bool = False):
    marker = require_fixture(original)
    if require_fixture(upgraded)["run_id"] != marker["run_id"]:
        raise AssertionError("Fixture lineage changed")
    before, after = original / "config", upgraded / "config"
    selected = "nekoray.json" if marker["source_core"] == 0 else "nekobox.json"
    for relative in marker["initial_files"]:
        prior, current = before / relative, after / relative
        if relative.startswith("profiles/"):
            node = read_json(prior)
            saved = read_json(current)
            retained = {key: value for key, value in node.items() if key not in ("yc", "report")}
            assert_subset(retained, saved, relative)
            expected_preference = marker["node_preferences_after_edit"][str(node["id"])] if preferences_edited else 0
            if saved.get("core_preference", 0) != expected_preference:
                raise AssertionError(f"Node preference changed: {relative}")
            results = json.loads(saved.get("test_results", "{}"))
            legacy = [entry for entry in results.get("history", []) if entry.get("status") == "legacy"]
            if not results.get("legacy_migrated") or len(legacy) != 1:
                raise AssertionError(f"Legacy history missing or duplicated: {relative}")
            assert_subset({"core": -1, "scope": "legacy", "latency_ms": node["yc"], "error": node["report"]}, legacy[0], relative + "/legacy")
            if saved.get("yc") != 0 or saved.get("report", "") != "" or results.get("latest"):
                raise AssertionError(f"Legacy result became authoritative: {relative}")
        elif relative == f"groups/{selected}":
            actual = read_json(current)
            assert_subset(read_json(prior), actual, relative)
            effective = actual.get("preferred_core", -1)
            if effective not in (0, 1):
                effective = marker["source_core"]
            expected = marker["default_preference_after_edit"] if preferences_edited else marker["source_core"]
            if effective != expected:
                raise AssertionError("Default preferred core changed")
        elif relative in ("groups/nekoray.json", "groups/nekobox.json") or relative.endswith("coreType") or relative.endswith(".txt"):
            if prior.read_bytes() != current.read_bytes():
                raise AssertionError(f"Unselected settings or auxiliary file changed: {relative}")
        else:
            assert_subset(read_json(prior), read_json(current), relative)
    expected_nodes = {path.name for path in (before / "profiles").glob("*.json")}
    actual_nodes = {path.name for path in (after / "profiles").glob("*.json")}
    if actual_nodes != expected_nodes:
        raise AssertionError("Profile IDs were added, removed or renumbered")


def stable_snapshot(root: Path) -> dict:
    marker = require_fixture(root)
    result = {}
    for relative in marker["initial_files"]:
        path = root / "config" / relative
        if relative.endswith(".json") or relative.startswith(("routes/", "routes_box/")):
            value = read_json(path)
            if relative in ("groups/nekoray.json", "groups/nekobox.json"):
                for key in ("mw_size", "splitter_state"):
                    value.pop(key, None)
            result[relative] = value
        else:
            result[relative] = path.read_text(encoding="utf-8-sig")
    return result
