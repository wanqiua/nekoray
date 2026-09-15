"""Synthetic PE fixtures; these tests never run an executable or a Go tool."""

import copy
import hashlib
import json
import struct
import unittest

from package_go_build_info import (BUILD_INFO_MAGIC, MODULE_INFO_END, MODULE_INFO_START,
                                   read_go_build_info, verify_build_info_notice, verify_go_metadata)


CORE_SPECS = {
    "nekoray_core": ("go1.21.13", [
        ("github.com/xtls/xray-core", "v1.8.4", "github.com/MatsuriDayo/Xray-core", "v0.0.0-20230829080406-01208225ee7e"),
    ]),
    "nekobox_core": ("go1.26.4", [
        ("github.com/sagernet/sing-box", "v1.14.0", "github.com/sagernet/sing-box", "v1.14.0"),
        ("github.com/anytls/sing-anytls", "v0.0.11", "github.com/anytls/sing-anytls", "v0.0.11"),
        ("github.com/sagernet/sing-snell", "v0.0.0-20260829071736-20f2eaec77c3", "github.com/sagernet/sing-snell", "v0.0.0-20260829071736-20f2eaec77c3"),
        ("github.com/sagernet/sing-quic", "v0.7.0-beta.4", "github.com/sagernet/sing-quic", "v0.7.0-beta.4"),
    ]),
}
SHARED_MODULE = ("github.com/matsuridayo/libneko", "v0.0.0-20230913024055-5277a5bfc889",
                 "github.com/matsuridayo/libneko", "v0.0.0-20230913024055-5277a5bfc889")
MODULE_FIELDS = ("module", "version", "source_module", "source_version")
PE_START = 0x80
SECTION_TABLE = PE_START + 24 + 240
DATA_START = 0x200


def module_text(core):
    rows = ["path\t" + core, "mod\t" + core + "\t(devel)\t"]
    for module, version, source_module, source_version in CORE_SPECS[core][1] + [SHARED_MODULE]:
        if (module, version) == (source_module, source_version):
            rows.append(f"dep\t{module}\t{version}\th1:synthetic-checksum")
        else:
            rows.extend((f"dep\t{module}\t{version}", f"=>\t{source_module}\t{source_version}\th1:synthetic-checksum", ""))
    rows.extend(("dep\tgrpc_server\tv1.0.0", "=>\t../../grpc_server\t(devel)\t", "",
                 "build\t-buildmode=exe", "build\t-compiler=gc", "build\tCGO_ENABLED=0",
                 "build\tGOOS=windows", "build\tGOARCH=amd64"))
    return "\n".join(rows) + "\n"


def length_string(value):
    length = len(value)
    prefix = bytearray()
    while length >= 128:
        prefix.append((length & 127) | 128)
        length >>= 7
    return bytes(prefix) + bytes([length]) + value


def synthetic_pe(core="nekobox_core", *, text=None, prefix=b"", framing=None):
    toolchain = CORE_SPECS[core][0].encode()
    module = (module_text(core) if text is None else text).encode()
    body = MODULE_INFO_START + module + MODULE_INFO_END if framing is None else framing
    data = prefix + BUILD_INFO_MAGIC + b"\x08\x02" + bytes(16) + length_string(toolchain) + length_string(body)
    raw_size = (len(data) + 511) // 512 * 512
    image = bytearray(DATA_START + raw_size)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, PE_START)
    image[PE_START:PE_START + 4] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", image, PE_START + 4, 0x8664, 1, 0, 0, 0, 240, 0x22)
    struct.pack_into("<H", image, PE_START + 24, 0x20b)
    struct.pack_into("<Q", image, PE_START + 24 + 24, 0x140000000)
    struct.pack_into("<8sIIIIIIHHI", image, SECTION_TABLE, b".data\x00\x00\x00", len(data), 0x2000,
                     raw_size, DATA_START, 0, 0, 0, 0, 0xc0000040)
    image[DATA_START:DATA_START + len(data)] = data
    return bytes(image)


def build_info_notice(core, text=None):
    body = module_text(core) if text is None else text
    return ("Go toolchain: " + CORE_SPECS[core][0] + "\n" +
            "\n".join("\t" + line for line in body.splitlines()) + "\n").encode()


def synthetic_go_payload():
    payload, cores, records = {}, {}, {}
    for core, (toolchain, dependencies) in CORE_SPECS.items():
        payload[core + ".exe"] = synthetic_pe(core)
        payload["licenses/" + core + "-build-info.txt"] = build_info_notice(core)
        cores[core] = {**dict(zip(MODULE_FIELDS, dependencies[0])), "go_version": toolchain,
                       "sha256": hashlib.sha256(payload[core + ".exe"]).hexdigest()}
        if core == "nekoray_core":
            cores[core]["source_revision"] = "01208225ee7e508044cca8eb6776a117bcecd997"
        for identity in dependencies + [SHARED_MODULE]:
            if identity in records:
                records[identity]["cores"].append(core)
                continue
            directory = "go/" + hashlib.sha256((identity[2] + "@" + identity[3]).encode()).hexdigest()[:16]
            record = {**dict(zip(MODULE_FIELDS, identity)), "cores": [core], "notices": directory,
                      "notice_files": 0 if identity == SHARED_MODULE else 1}
            prefix = "licenses/" + directory + "/"
            if identity == SHARED_MODULE:
                record["source_notice"] = "Upstream snapshot has no standalone LICENSE; original README retained."
                payload[prefix + "SOURCE-NOTICE.txt"] = b"Synthetic source notice"
                payload[prefix + "README.md"] = b"Synthetic retained upstream README"
            else:
                payload[prefix + "LICENSE"] = b"Synthetic upstream license"
            records[identity] = record
    payload["licenses/go-modules.json"] = json.dumps(list(records.values())).encode()
    return payload, cores


class GoBuildInfoTests(unittest.TestCase):
    def test_both_toolchains_preserve_external_replacement_and_local_module(self):
        for core in CORE_SPECS:
            with self.subTest(core=core):
                info = read_go_build_info(synthetic_pe(core))
                self.assertEqual(info.toolchain, CORE_SPECS[core][0])
                self.assertEqual(info.path, core)
                self.assertEqual(info.text, module_text(core))
                self.assertEqual(info.dependencies[0]["source_module"], CORE_SPECS[core][1][0][2])
                self.assertEqual(info.dependencies[-1]["source_module"], "../../grpc_server")
                self.assertEqual(info.settings["GOARCH"], "amd64")

    def test_bom_and_windows_newlines_do_not_change_notice_metadata(self):
        notice = b"\xef\xbb\xbf" + build_info_notice("nekoray_core").replace(b"\n", b"\r\n")
        verify_build_info_notice(read_go_build_info(synthetic_pe("nekoray_core")), notice)

    def test_non_pe_and_truncated_headers_are_rejected(self):
        valid = synthetic_pe()
        for image in (b"not a PE", valid[:63], valid[:PE_START + 20], valid[:SECTION_TABLE + 39]):
            with self.subTest(length=len(image)), self.assertRaises(ValueError):
                read_go_build_info(image)

    def test_wrong_machine_and_optional_header_are_rejected(self):
        for offset, value in ((PE_START + 4, 0x14c), (PE_START + 24, 0x10b), (PE_START + 20, 2)):
            image = bytearray(synthetic_pe())
            struct.pack_into("<H", image, offset, value)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "amd64 PE32"):
                read_go_build_info(bytes(image))

    def test_section_cannot_overlap_headers_or_extend_past_file(self):
        for offset, value in ((SECTION_TABLE + 20, PE_START), (SECTION_TABLE + 16, 0xffffffff)):
            image = bytearray(synthetic_pe())
            struct.pack_into("<I", image, offset, value)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "section is outside"):
                read_go_build_info(bytes(image))

    def test_magic_in_executable_section_or_overlay_is_not_metadata(self):
        image = bytearray(synthetic_pe())
        struct.pack_into("<I", image, SECTION_TABLE + 36, 0x60000020)
        with self.assertRaisesRegex(ValueError, "no build-info data section"):
            read_go_build_info(bytes(image))
        image = bytearray(synthetic_pe())
        struct.pack_into("<I", image, SECTION_TABLE + 8, 16)
        with self.assertRaisesRegex(ValueError, "no aligned build-info"):
            read_go_build_info(bytes(image))

    def test_alignment_is_checked_against_section_virtual_address(self):
        self.assertEqual(read_go_build_info(synthetic_pe(prefix=bytes(16))).path, "nekobox_core")
        with self.assertRaisesRegex(ValueError, "no aligned build-info"):
            read_go_build_info(synthetic_pe(prefix=b"x"))

    def test_pre_inline_or_wrong_pointer_metadata_is_rejected(self):
        for offset, value in ((DATA_START + 15, 0), (DATA_START + 14, 4)):
            image = bytearray(synthetic_pe())
            image[offset] = value
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "supported inline"):
                read_go_build_info(bytes(image))

    def test_varint_overflow_and_length_past_section_are_rejected(self):
        for length in (b"\xff" * 10, b"\xff" * 8 + b"\x01"):
            image = bytearray(synthetic_pe())
            image[DATA_START + 32:DATA_START + 32 + len(length)] = length
            with self.subTest(length=length.hex()), self.assertRaisesRegex(ValueError, "length|string exceeds"):
                read_go_build_info(bytes(image))

    def test_truncated_varint_is_rejected(self):
        image = bytearray(synthetic_pe())
        struct.pack_into("<I", image, SECTION_TABLE + 8, 33)
        image[DATA_START + 32] = 0x80
        with self.assertRaisesRegex(ValueError, "Truncated"):
            read_go_build_info(bytes(image))

    def test_module_framing_and_utf8_are_checked(self):
        for framing in (b"invalid frame", bytes(16) + b"path\tx\n" + MODULE_INFO_END,
                        MODULE_INFO_START + b"\xff\n" + MODULE_INFO_END):
            with self.subTest(framing=framing), self.assertRaises(ValueError):
                read_go_build_info(synthetic_pe(framing=framing))

    def test_orphan_replacements_and_duplicate_records_are_rejected(self):
        text = module_text("nekobox_core")
        for altered in ("=>\texample.test/dependency\tv1.0.0\n" + text,
                        text + "dep\tgrpc_server\tv1.0.0\n", text + "build\tGOOS=windows\n",
                        text + "path\tnekobox_core\n"):
            with self.subTest(altered=altered[-80:]), self.assertRaises(ValueError):
                read_go_build_info(synthetic_pe(text=altered))

    def test_replacement_requires_checksum_column_even_for_local_module(self):
        text = module_text("nekobox_core").replace("=>\t../../grpc_server\t(devel)\t\n", "=>\t../../grpc_server\t(devel)\n")
        with self.assertRaisesRegex(ValueError, "Malformed Go build-info record"):
            read_go_build_info(synthetic_pe(text=text))

    def test_quoted_or_whitespace_build_keys_cannot_hide_duplicate_settings(self):
        for name in ('"GOARCH"', "bad key", "`GOARCH`"):
            text = module_text("nekobox_core") + f"build\t{name}=arm64\n"
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Invalid or duplicate"):
                read_go_build_info(synthetic_pe(text=text))

    def test_actual_embedded_checksum_must_match_notice(self):
        notice = build_info_notice("nekobox_core").replace(b"synthetic-checksum", b"old-checksum", 1)
        with self.assertRaisesRegex(ValueError, "embedded metadata"):
            verify_build_info_notice(read_go_build_info(synthetic_pe()), notice)

    def test_complete_notices_are_deduplicated_without_counting_local_sources(self):
        payload, cores = synthetic_go_payload()
        report = verify_go_metadata(payload.__getitem__, set(payload), cores, json.loads(payload["licenses/go-modules.json"]))
        self.assertTrue(report["go_build_info_verified"])
        self.assertTrue(report["go_module_notices_verified"])
        self.assertEqual(report["go_modules_verified"], 6)
        self.assertEqual(report["go_core_metadata"]["nekoray_core"]["external_dependencies"], 2)
        self.assertEqual(report["go_core_metadata"]["nekobox_core"]["external_dependencies"], 5)

    def test_unknown_local_replacement_requires_source_review(self):
        payload, cores = synthetic_go_payload()
        changed = module_text("nekobox_core").replace("../../grpc_server", "../../unknown_module")
        payload["nekobox_core.exe"] = synthetic_pe(text=changed)
        payload["licenses/nekobox_core-build-info.txt"] = build_info_notice("nekobox_core", changed)
        with self.assertRaisesRegex(ValueError, "local Go dependency"):
            verify_go_metadata(payload.__getitem__, set(payload), cores, json.loads(payload["licenses/go-modules.json"]))

    def test_notice_identity_membership_and_directory_are_validated(self):
        payload, cores = synthetic_go_payload()
        original = json.loads(payload["licenses/go-modules.json"])
        mutations = [lambda rows: rows.pop(), lambda rows: rows.append(copy.deepcopy(rows[0])),
                     lambda rows: rows[0].update(version="v1.8.3"),
                     lambda rows: rows[0].update(cores=["nekobox_core"]),
                     lambda rows: rows[0].update(cores=["nekoray_core", "nekoray_core"]),
                     lambda rows: rows[0].update(notices="go/" + "0" * 16),
                     lambda rows: rows[0].update(notice_files=True)]
        for mutation in mutations:
            rows = copy.deepcopy(original)
            mutation(rows)
            with self.subTest(rows=rows[0]), self.assertRaises(ValueError):
                verify_go_metadata(payload.__getitem__, set(payload), cores, rows)

    def test_core_provenance_is_tied_to_embedded_toolchain_and_replacement(self):
        payload, original = synthetic_go_payload()
        for changes in ({"go_version": "go1.26.1"}, {"source_version": "v1.8.4"}, {"module": "wrong.example/core"}):
            cores = copy.deepcopy(original)
            cores["nekoray_core"].update(changes)
            with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, "provenance"):
                verify_go_metadata(payload.__getitem__, set(payload), cores, json.loads(payload["licenses/go-modules.json"]))

    def test_zero_license_exception_requires_original_source_note_and_readme(self):
        payload, cores = synthetic_go_payload()
        rows = json.loads(payload["licenses/go-modules.json"])
        missing = next(path for path in payload if path.endswith("SOURCE-NOTICE.txt"))
        del payload[missing]
        with self.assertRaisesRegex(ValueError, "source notice and README"):
            verify_go_metadata(payload.__getitem__, set(payload), cores, rows)

    def test_unlisted_and_empty_notice_files_are_rejected(self):
        original, cores = synthetic_go_payload()
        for name, value in (("licenses/go/0000000000000000/LICENSE", b"orphan"),
                            (next(path for path in original if path.endswith("/LICENSE")), b"")):
            payload = {**original, name: value}
            with self.subTest(name=name), self.assertRaises(ValueError):
                verify_go_metadata(payload.__getitem__, set(payload), cores, json.loads(payload["licenses/go-modules.json"]))


if __name__ == "__main__":
    unittest.main()
