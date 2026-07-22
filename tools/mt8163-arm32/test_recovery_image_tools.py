#!/usr/bin/env python3
"""Fail-closed unit tests for the MT8163 recovery image tools."""

from __future__ import annotations

import importlib.util
import stat
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parent


def load_tool(name: str):
    path = TOOLS_DIR / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"mt8163_{name}", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


builder = load_tool("build_recovery_image")
verifier = load_tool("verify_recovery_image")


def newc_member(name: bytes, mode: int = stat.S_IFREG | 0o644,
                payload: bytes = b"") -> bytes:
    name_field = name + b"\0"
    values = (
        1, mode, 0, 0, 1, 0, len(payload), 0, 0, 0, 0, len(name_field), 0,
    )
    header = b"070701" + b"".join(f"{value:08x}".encode() for value in values)
    record = header + name_field
    record += b"\0" * (-len(record) & 3)
    record += payload
    record += b"\0" * (-len(record) & 3)
    return record


def newc_archive(*members: bytes, tail: bytes = b"") -> bytes:
    return b"".join(members) + newc_member(b"TRAILER!!!", 0) + tail


class NewcTests(unittest.TestCase):
    def test_canonical_member_and_zero_padding(self) -> None:
        entries = verifier.parse_newc(
            newc_archive(newc_member(b"./foo", payload=b"value"), tail=b"\0" * 17)
        )
        self.assertEqual(entries["foo"].data, b"value")

    def test_unsafe_or_ambiguous_names_are_rejected(self) -> None:
        unsafe_names = (
            b"/absolute", b"../escape", b"a/../escape", b"././alias",
            b"a//alias", b"a/./alias", b"interior\0nul",
        )
        for name in unsafe_names:
            with self.subTest(name=name), self.assertRaises(SystemExit):
                verifier.parse_newc(newc_archive(newc_member(name)))

    def test_duplicate_normalized_member_is_rejected(self) -> None:
        archive = newc_archive(newc_member(b"foo"), newc_member(b"./foo"))
        with self.assertRaises(SystemExit):
            verifier.parse_newc(archive)

    def test_duplicate_trailer_and_nonzero_tail_are_rejected(self) -> None:
        for archive in (
            newc_archive() + newc_member(b"TRAILER!!!", 0),
            newc_archive(tail=b"\x01"),
            newc_member(b"TRAILER!!!", 0) + newc_member(b"late"),
        ):
            with self.subTest(size=len(archive)), self.assertRaises(SystemExit):
                verifier.parse_newc(archive)


class SymlinkTests(unittest.TestCase):
    @staticmethod
    def entry(name: str, mode: int, payload: bytes = b""):
        return verifier.Entry(name, mode, 0, 0, 0, payload)

    def test_relative_in_tree_symlink_is_accepted(self) -> None:
        entries = {
            "bin": self.entry("bin", stat.S_IFDIR | 0o755),
            "bin/target": self.entry("bin/target", stat.S_IFREG | 0o755),
            "bin/link": self.entry("bin/link", stat.S_IFLNK | 0o777, b"target"),
        }
        verifier.validate_symlinks(entries)

    def test_absolute_escape_dangling_and_loop_are_rejected(self) -> None:
        cases = (
            {"link": self.entry("link", stat.S_IFLNK | 0o777, b"/outside")},
            {"nested/link": self.entry("nested/link", stat.S_IFLNK | 0o777, b"../../outside")},
            {"link": self.entry("link", stat.S_IFLNK | 0o777, b"missing")},
            {
                "one": self.entry("one", stat.S_IFLNK | 0o777, b"two"),
                "two": self.entry("two", stat.S_IFLNK | 0o777, b"one"),
            },
        )
        for entries in cases:
            with self.subTest(entries=tuple(entries)), self.assertRaises(SystemExit):
                verifier.validate_symlinks(entries)

    def test_member_beneath_symlink_parent_is_rejected(self) -> None:
        entries = {
            "real": self.entry("real", stat.S_IFDIR | 0o755),
            "alias": self.entry("alias", stat.S_IFLNK | 0o777, b"real"),
            "alias/file": self.entry("alias/file", stat.S_IFREG | 0o644),
        }
        with self.assertRaises(SystemExit):
            verifier.validate_archive_tree(entries)


class SourceTests(unittest.TestCase):
    def test_pinned_source_rejects_symlink_components(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "real").mkdir()
            (root / "real/file").write_bytes(b"pinned")
            (root / "alias").symlink_to("real", target_is_directory=True)
            self.assertEqual(
                builder.pinned_source(root, "real/file", "test"), root / "real/file"
            )
            with self.assertRaises(SystemExit):
                builder.pinned_source(root, "alias/file", "test")
            (root / "file-link").symlink_to("real/file")
            with self.assertRaises(SystemExit):
                builder.pinned_source(root, "file-link", "test")

    def test_pinned_source_rejects_noncanonical_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in ("../file", "/file", "a//file", "a/./file"):
                with self.subTest(relative=relative), self.assertRaises(SystemExit):
                    builder.pinned_source(root, relative, "test")


class PatchRouteContractTests(unittest.TestCase):
    def test_stock_route_contract_is_shared_and_decodes_exactly(self) -> None:
        expected = {
            "lib/firmware/ROMv2_lm_patch_1_0_hdr.bin": (
                bytes.fromhex("8a00"), bytes.fromhex("22000600"), 2,
                bytes.fromhex("00000600"),
            ),
            "lib/firmware/ROMv2_lm_patch_1_1_hdr.bin": (
                bytes.fromhex("8a00"), bytes.fromhex("21000ef0"), 1,
                bytes.fromhex("00000ef0"),
            ),
        }
        self.assertEqual(builder.CONNECTIVITY_PATCH_ROUTES, expected)
        self.assertEqual(verifier.CONNECTIVITY_PATCH_ROUTES, expected)
        for _name, (_header, route, sequence, address) in expected.items():
            self.assertEqual(route[0] >> 4, 2)
            self.assertEqual(route[0] & 0x0F, sequence)
            self.assertEqual(b"\0" + route[1:], address)

    def test_route_manifest_rejects_boolean_sequence(self) -> None:
        expected = {
            "patch": {
                "header": "8a00",
                "route": "21000ef0",
                "patch_count": 2,
                "download_seq": 1,
                "address": "00000ef0",
            }
        }
        changed = {"patch": {**expected["patch"], "download_seq": True}}
        self.assertFalse(verifier.strictly_equal(changed, expected))
        self.assertTrue(verifier.strictly_equal(expected, expected))


class PolicyTests(unittest.TestCase):
    @staticmethod
    def control(name: str, payload: bytes):
        return verifier.Entry(name, stat.S_IFREG | 0o644, 0, 0, 0, payload)

    def test_android_init_wifi_activation_is_rejected(self) -> None:
        entries = {"rogue.rc": self.control("rogue.rc", b"write\t/dev/wmtWifi 1\n")}
        with self.assertRaises(SystemExit):
            verifier.validate_no_connectivity_autostart(entries)

    def test_device_node_setup_is_not_activation(self) -> None:
        entries = {
            "libreecho-init": self.control(
                "libreecho-init", b"mknod /dev/wmtWifi c 190 0\nchmod 0660 /dev/wmtWifi\n"
            )
        }
        verifier.validate_no_connectivity_autostart(entries)

    def test_wifi_activation_is_deferred_until_adb_ready(self) -> None:
        source = (TOOLS_DIR / "initramfs/libreecho-init").read_text()
        self.assertLess(
            source.index("log init-ready-pid1-managed"),
            source.index("start_wifi_network &"),
        )
        self.assertIn("wifi-network-worker-started-after-adb", source)
        self.assertIn("reboot-supervisor-started", source)
        self.assertIn("/tmp/reboot.request", source)
        self.assertIn("runme-timeout", source)
        self.assertIn("/tmp/runme.cancel", source)
        self.assertIn("wmt_stock_compat", source)
        self.assertIn("--no-function-on", source)
        self.assertIn("--ok --once", source)
        self.assertIn("pidof wmt_launcher", source)
        self.assertIn("timeout 30", source)
        self.assertIn("/sbin/libreecho-wifi", source)
        self.assertIn("/etc/udhcpc.script", (TOOLS_DIR / "initramfs/libreecho-wifi").read_text())
        self.assertNotIn("/system/vendor/bin/wmt_loader >/tmp/wifi-wmt-loader.log", source)

    def test_schema2_disabled_record_is_exact(self) -> None:
        record = {
            "id": verifier.CONNECTIVITY_BUNDLE_ID,
            "enabled": False,
            "activation": "manual-gates-only",
            "autostart": False,
            "files": {},
            "helpers": {},
            "symlinks": {},
        }
        self.assertFalse(verifier.validate_connectivity({}, {"connectivity": record}, 2))
        changed = {**record, "autostart": True}
        with self.assertRaises(SystemExit):
            verifier.validate_connectivity({}, {"connectivity": changed}, 2)

    def test_boolean_manifest_schema_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            verifier.manifest_schema({"schema_version": True})


class MkimgHeaderTests(unittest.TestCase):
    """Regression: LK rejects a KERNEL header whose name lacks a NUL byte."""

    @staticmethod
    def header(name_suffix: bytes = b"\x00\x00") -> bytes:
        hdr = bytearray(512)
        hdr[0:4] = bytes.fromhex("88168858")
        hdr[4:8] = (1024).to_bytes(4, "little")
        hdr[8:14] = b"KERNEL"
        hdr[14:14 + len(name_suffix)] = name_suffix
        return bytes(hdr)

    def test_null_terminated_name_is_accepted(self) -> None:
        verifier.validate_mkimg_header(self.header(b"\x00\x00"))

    def test_ff_filled_name_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            verifier.validate_mkimg_header(self.header(b"\xff\xff"))

    def test_missing_null_terminator_is_rejected(self) -> None:
        hdr = bytearray(self.header(b"\x00\x00"))
        hdr[14] = 0x41  # 'A' instead of NUL
        with self.assertRaises(SystemExit):
            verifier.validate_mkimg_header(bytes(hdr))

    def test_bad_magic_is_rejected(self) -> None:
        hdr = bytearray(self.header())
        hdr[0:4] = b"\x00\x00\x00\x00"
        with self.assertRaises(SystemExit):
            verifier.validate_mkimg_header(bytes(hdr))

    def test_wrong_name_is_rejected(self) -> None:
        hdr = bytearray(self.header())
        hdr[8:14] = b"ROOTFS"
        with self.assertRaises(SystemExit):
            verifier.validate_mkimg_header(bytes(hdr))


if __name__ == "__main__":
    unittest.main()
