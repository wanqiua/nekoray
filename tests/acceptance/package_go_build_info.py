"""Read Go metadata from packaged PE files without executing a core or Go tool.

The two supported core toolchains use Go 1.18+'s inline build-info format.
The PE section selection and string framing follow Go's debug/buildinfo reader.
This deliberately rejects older pointer-based metadata and non-amd64 payloads.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import re
import struct
from typing import Callable


BUILD_INFO_MAGIC = b"\xff Go buildinf:"
MODULE_INFO_START = bytes.fromhex("3077af0c9274080241e1c107e6d618e6")
MODULE_INFO_END = bytes.fromhex("f932433186182072008242104116d8f2")
MODULE_FIELDS = ("module", "version", "source_module", "source_version")
CORE_NAMES = ("nekoray_core", "nekobox_core")
BOX_PROTOCOL_MODULES = {
    "github.com/anytls/sing-anytls", "github.com/sagernet/sing-snell", "github.com/sagernet/sing-quic",
}
LIBNEKO_SOURCE = ("github.com/matsuridayo/libneko", "v0.0.0-20230913024055-5277a5bfc889")


@dataclass(frozen=True)
class GoBuildInfo:
    toolchain: str
    text: str
    path: str
    main: dict[str, str]
    dependencies: tuple[dict[str, str], ...]
    settings: dict[str, str]


def _length_string(data: bytes, offset: int) -> tuple[bytes, int]:
    length = 0
    for index in range(10):
        if offset >= len(data):
            raise ValueError("Truncated Go build-info string length")
        byte = data[offset]
        offset += 1
        if index == 9 and byte > 1:
            raise ValueError("Overflowing Go build-info string length")
        length |= (byte & 0x7f) << (index * 7)
        if byte < 0x80:
            if length > len(data) - offset:
                raise ValueError("Go build-info string exceeds its PE section")
            return data[offset:offset + length], offset + length
    raise ValueError("Invalid Go build-info string length")


def _module(fields: list[str]) -> dict[str, str]:
    if (len(fields) not in (3, 4) or
        any(not value or any(character.isspace() for character in value) for value in fields[1:3])):
        raise ValueError("Malformed Go build-info module record")
    return dict(zip(MODULE_FIELDS, (fields[1], fields[2], fields[1], fields[2])))


def parse_module_info(toolchain: str, text: str) -> GoBuildInfo:
    path, main, previous = None, None, None
    dependencies = []
    module_names = set()
    settings = {}
    if not re.fullmatch(r"go1\.\d+(?:\.\d+)?(?:[a-z]+\d+)?", toolchain) or not text.endswith("\n") or "\x00" in text:
        raise ValueError("Invalid Go toolchain or module information")
    for line in text.splitlines():
        if not line:
            previous = None
            continue
        fields = line.split("\t")
        kind = fields[0]
        if kind == "path" and len(fields) == 2 and fields[1] and path is None:
            path = fields[1]
            previous = None
        elif kind == "mod" and main is None:
            main = _module(fields)
            previous = main
        elif kind == "dep":
            dependency = _module(fields)
            if dependency["module"] in module_names:
                raise ValueError("Duplicate Go build-info dependency")
            module_names.add(dependency["module"])
            dependencies.append(dependency)
            previous = dependency
        elif kind == "=>" and previous is not None and len(fields) == 4:
            replacement = _module(fields)
            previous.update(source_module=replacement["module"], source_version=replacement["version"])
            previous = None
        elif kind == "build" and len(fields) == 2:
            name, separator, value = fields[1].partition("=")
            if (not name or not separator or name in settings or
                any(character.isspace() or character in '\"`' for character in name)):
                raise ValueError("Invalid or duplicate Go build-info setting")
            settings[name] = value
            previous = None
        else:
            raise ValueError("Malformed Go build-info record")
    if path is None or main is None or not dependencies:
        raise ValueError("Go build-info has no main module or dependencies")
    return GoBuildInfo(toolchain, text, path, main, tuple(dependencies), settings)


def read_go_build_info(executable: bytes) -> GoBuildInfo:
    if len(executable) < 64 or executable[:2] != b"MZ":
        raise ValueError("Go core is not a PE executable")
    pe = struct.unpack_from("<I", executable, 60)[0]
    if pe < 64 or pe + 24 > len(executable) or executable[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("Invalid Go core PE header")
    machine, sections = struct.unpack_from("<HH", executable, pe + 4)
    optional_size = struct.unpack_from("<H", executable, pe + 20)[0]
    optional = pe + 24
    section_table = optional + optional_size
    table_end = section_table + sections * 40
    if (machine != 0x8664 or not 1 <= sections <= 96 or optional_size < 112 or table_end > len(executable) or
        struct.unpack_from("<H", executable, optional)[0] != 0x20b):
        raise ValueError("Go core must be a complete Windows amd64 PE32+ executable")
    image_base = struct.unpack_from("<Q", executable, optional + 24)[0]
    data = None
    address = 0
    for index in range(sections):
        start = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw = struct.unpack_from("<IIII", executable, start + 8)
        flags = struct.unpack_from("<I", executable, start + 36)[0]
        if raw_size and (raw < table_end or raw_size > len(executable) - raw):
            raise ValueError("Go core PE section is outside its executable")
        # This is Go debug/buildinfo.peExe.DataStart's first writable,
        # initialized non-executable data section, allowing alignment bits.
        if (data is None and virtual_address and raw_size and flags & ~0x600000 == 0xc0000040):
            data = executable[raw:raw + min(raw_size, virtual_size)]
            address = image_base + virtual_address
    if data is None:
        raise ValueError("Go core has no build-info data section")
    position = data.find(BUILD_INFO_MAGIC)
    while position >= 0 and (address + position) % 16:
        position = data.find(BUILD_INFO_MAGIC, position + 1)
    if position < 0 or position + 32 > len(data):
        raise ValueError("Go core has no aligned build-info header")
    if data[position + 14:position + 16] != b"\x08\x02":
        raise ValueError("Go core does not use supported inline amd64 build-info")
    toolchain, position = _length_string(data, position + 32)
    module_info, _ = _length_string(data, position)
    if (len(module_info) < 33 or not module_info.startswith(MODULE_INFO_START) or
        not module_info.endswith(MODULE_INFO_END) or module_info[-17] != 10):
        raise ValueError("Go module information has invalid framing")
    try:
        return parse_module_info(toolchain.decode("utf-8"), module_info[16:-16].decode("utf-8"))
    except UnicodeDecodeError as error:
        raise ValueError("Go build-info is not valid UTF-8") from error


def verify_build_info_notice(info: GoBuildInfo, notice: bytes):
    try:
        lines = notice.decode("utf-8-sig").splitlines()
    except UnicodeDecodeError as error:
        raise ValueError("Go build-info notice is not valid UTF-8") from error
    if not lines or lines[0] != "Go toolchain: " + info.toolchain or any(not line.startswith("\t") for line in lines[1:]):
        raise ValueError("Go build-info notice does not match its embedded toolchain or format")
    if "\n".join(line[1:] for line in lines[1:]) + "\n" != info.text:
        raise ValueError("Go build-info notice differs from the core's embedded metadata")


def verify_go_metadata(read_file: Callable[[str], bytes], files: set[str], cores: dict, notices: object) -> dict:
    expected = {}
    summary = {}
    for core in CORE_NAMES:
        try:
            info = read_go_build_info(read_file(core + ".exe"))
            verify_build_info_notice(info, read_file("licenses/" + core + "-build-info.txt"))
        except ValueError as error:
            raise ValueError(f"{core}: {error}") from error
        if info.path != core or info.main["module"] != core or cores[core].get("go_version") != info.toolchain:
            raise ValueError(f"{core}: Core provenance differs from embedded main module or toolchain")
        for name, value in (("GOOS", "windows"), ("GOARCH", "amd64"), ("-buildmode", "exe"), ("CGO_ENABLED", "0")):
            if info.settings.get(name) != value:
                raise ValueError(f"{core}: Unexpected embedded Go build target")
        external = 0
        matching_core = False
        for dependency in info.dependencies:
            if dependency["source_version"] == "(devel)":
                if tuple(dependency[field] for field in MODULE_FIELDS) != ("grpc_server", "v1.0.0", "../../grpc_server", "(devel)"):
                    raise ValueError(f"{core}: Unrecognized local Go dependency requires source review")
                continue
            key = tuple(dependency[field] for field in MODULE_FIELDS)
            expected.setdefault(key, set()).add(core)
            external += 1
            if all(dependency[field] == cores[core].get(field) for field in MODULE_FIELDS):
                matching_core = True
        if not matching_core:
            raise ValueError(f"{core}: Core module or replacement differs from its provenance")
        if core == "nekobox_core" and not BOX_PROTOCOL_MODULES <= {item["module"] for item in info.dependencies}:
            raise ValueError("sing-box core is missing a required AnyTLS, Snell or QUIC dependency")
        summary[core] = {"toolchain": info.toolchain, "external_dependencies": external}
    if not isinstance(notices, list):
        raise ValueError("Go module notices must be a list")
    observed = {}
    sources = set()
    covered_files = set()
    for record in notices:
        if not isinstance(record, dict) or any(not isinstance(record.get(field), str) or not record[field] for field in MODULE_FIELDS):
            raise ValueError("Invalid Go module notice identity")
        key = tuple(record[field] for field in MODULE_FIELDS)
        source_key = key[2] + "@" + key[3]
        if key in observed or source_key in sources:
            raise ValueError("Duplicate Go module notice record")
        sources.add(source_key)
        linked_cores = record.get("cores")
        if (not isinstance(linked_cores, list) or not linked_cores or
            any(core not in CORE_NAMES for core in linked_cores) or len(set(linked_cores)) != len(linked_cores)):
            raise ValueError("Invalid Go module notice core list")
        observed[key] = set(linked_cores)
        directory = record.get("notices")
        count = record.get("notice_files")
        if not isinstance(directory, str) or not re.fullmatch(r"go/[0-9a-f]{16}", directory) or type(count) is not int or count < 0:
            raise ValueError("Invalid Go module notice directory or file count")
        if directory != "go/" + hashlib.sha256(source_key.encode("utf-8")).hexdigest()[:16]:
            raise ValueError("Go module notice directory differs from its source identity")
        prefix = "licenses/" + directory + "/"
        members = {path for path in files if path.startswith(prefix)}
        if count == 0:
            # This reviewed upstream snapshot has no standalone LICENSE. Its
            # original README and explicit source notice are retained instead.
            if (key[2:] != LIBNEKO_SOURCE or not isinstance(record.get("source_notice"), str) or not record["source_notice"].strip() or
                members != {prefix + "README.md", prefix + "SOURCE-NOTICE.txt"}):
                raise ValueError("Zero-license Go module lacks its reviewed source notice and README")
        elif len(members) != count:
            raise ValueError("Go module notice files are missing or differ from their recorded count")
        if any(not read_file(path).strip() for path in members):
            raise ValueError("Go module contains an empty notice file")
        covered_files.update(members)
    if observed != expected:
        raise ValueError("Go module notices differ from linked modules, versions, replacements or core membership")
    if covered_files != {path for path in files if path.startswith("licenses/go/")}:
        raise ValueError("Go notice directory contains unlisted module files")
    return {"go_build_info_verified": True, "go_module_notices_verified": True,
            "go_modules_verified": len(expected), "go_core_metadata": summary}
