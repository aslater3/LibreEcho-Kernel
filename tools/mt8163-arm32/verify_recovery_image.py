#!/usr/bin/env python3
"""Independent verifier for the MT8163 ARM32 recovery boot image."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import stat
import struct
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


ANDROID_MAGIC = b"ANDROID!"
MKIMG_MAGIC = bytes.fromhex("88168858")
FDT_MAGIC = bytes.fromhex("d00dfeed")
PAGE = 0x800
MKIMG_SIZE = 0x200
IMAGE_SIZE = 0x1000000
KERNEL_ADDR = 0x40008000
RAMDISK_ADDR = 0x43478000
RAMDISK_END_LIMIT = 0x44000000
TAGS_ADDR = 0x48000000
ATF_START = 0x43000000
ATF_END = 0x43030000
DTB_SIZE = 0x10000
ZIMAGE_MAGIC = 0x016F2818
SOURCE_SHA256 = "c0f52a3b079d214495cd3dd22f92fd85695d1b868c58b491a2edb933bc4f6d1a"
ZIMAGE_SHA256 = "4e144959eb0ffaee91b37d05a0f871863a74f4abb1bad0474c2fec358d5176a6"
SYSTEM_MAP_SHA256 = "527292112edd28e8facf2998eefe2224b08a05b193efc73634cd998e9113ba95"
CONNECTIVITY_BUNDLE_ID = "mt8163-v181-stock-v1"
CONNECTIVITY_STOCK_SYSTEM_SHA256 = "56540b3a9ac4437901a5510d9fb5e09b1a8d0cc229548f0b08bb5c22d78684fe"
CONNECTIVITY_EVIDENCE_MANIFEST_SHA256 = "d1eedd04efe0dbc78853f2b0f9357c092b4ca66242648908c0369956538441eb"
STOCK_DTB_SHA256 = "f44630ba28f503dd7503bc7cffa2ee96a319acf2f58f1456bb6f5ff23d57dee1"
PADDED_STOCK_DTB_SHA256 = "08b16ec39554d644d8cbdf8f5816559f85414ab45bc1901de46a7cd43dc286ed"
BUSYBOX_SHA256 = "d4c8fd2aea01abd851c703f39b29c0de748b2751e4e1a85cae570fa53ad8f4fb"
LOADER_SHA256 = "1063871174f1bd4f08f4d330e20b07aeb0820327ee739a4d8d1b644df842cb6b"
INIT_SHA256 = "0564299ebbdd4b76fc00b7f48b434355b484874c2ad013f7c6a3dc5cbd103df7"
ADBD_SHA256 = "1c0d14afb1ce19494ee1da935e1076f49ff57e359d348262a28bb3d56abeb930"
OVERLAY_FILES = {
    "default.prop": 0o644,
    "init.rc": 0o644,
    "init.recovery.mt8163.rc": 0o644,
    "libreecho-init": 0o755,
}

CONNECTIVITY_FILES = {
    "system/bin/linker": (
        0o755, 630460, "73dc93e06a9ce0a76b5353f2c282f1ac3dd0dccd0e8e7f06fc20e5433ef4a3dc", (),
    ),
    "system/vendor/bin/wmt_loader": (
        0o755, 17992, "de9ee285a09a7db5b079233f7c9129c5484ecb6701b54da45e2a29f310e74ff9",
        ("libcutils.so", "libc++.so", "libdl.so", "libc.so", "libm.so"),
    ),
    "system/vendor/bin/wmt_launcher": (
        0o755, 31448, "1f34425d727ea64524c9edaeac5e6b295df7a6054703dcc79b164021560252e5",
        ("libcutils.so", "libc++.so", "libdl.so", "libc.so", "libm.so"),
    ),
    "lib/firmware/ROMv2_lm_patch_1_0_hdr.bin": (
        0o644, 128720, "b4460117f51a43f3284594ec08d8c8861ecc0e42b17820987da03ecabdebac1e", None,
    ),
    "lib/firmware/ROMv2_lm_patch_1_1_hdr.bin": (
        0o644, 50148, "10c4ed22a10b8a136bffd7ffce4d552300d76f8e593627d2a9841c3b11a5697e", None,
    ),
    "lib/firmware/WIFI_RAM_CODE_8163": (
        0o644, 373840, "9669cc9b03cfdc5e8fd4fd6e14c4c4050e8c196738ca4707eea12f14a6a8e64c", None,
    ),
    "lib/firmware/WMT_SOC.cfg": (
        0o644, 119, "302bd4462de99c028c04092e561c1500d65582ce42a93c4c72ccae6e2c99013d", None,
    ),
    "system/lib/libcutils.so": (
        0o644, 104436, "dcf249ceed2c84ab45454ff8fd3fa0624248b410962c4ea9e9e799610192542b",
        ("liblog.so", "libc++.so", "libdl.so", "libc.so", "libm.so"),
    ),
    "system/lib/libc++.so": (
        0o644, 575068, "38f15c7897307e65c9b9a13174782e7b79146e453b8b80e09128aae8b6ab1df5",
        ("libdl.so", "libc.so", "libm.so"),
    ),
    "system/lib/libdl.so": (
        0o644, 13640, "efb8d634212b215b53f8c95f2b8372e9139ee13dc74717b7d25999de97d5b1cc", (),
    ),
    "system/lib/libc.so": (
        0o644, 780476, "1254edac10625b1e7e123c20ea8d8f3175ad07014c9ddcca7bb3ea74db555357",
        ("libdl.so",),
    ),
    "system/lib/libm.so": (
        0o644, 132820, "3703abfae55405f1ca876cfaf5c8e41b0dafdd30d4ecec88cbd1100c5b0341ed",
        ("libc.so",),
    ),
    "system/lib/liblog.so": (
        0o644, 67460, "84e34e101618dae346cefca70c8cd866b92e6bcdec64246a130dcd12560410c0",
        ("libc.so", "libm.so"),
    ),
}

CONNECTIVITY_REFERENCE_FILES = {
    "init.connectivity.rc": {
        "sha256": "142c3f2239255dff573196daaf7da00687be9c5c54174dcbecfa309074d9d379",
        "size": 3167,
    },
    "ueventd.mt8163.rc": {
        "sha256": "b1d212a42d213b4b1412648e7501baf55aa3ee653236cdf10f650050e0ea325c",
        "size": 4255,
    },
}

CONNECTIVITY_SYMLINKS = {
    "vendor": "system/vendor",
    "system/vendor/firmware": "../../lib/firmware",
    "system/etc/firmware": "../../lib/firmware",
    "etc/firmware": "../lib/firmware",
    "lib/firmware/WIFI_RAM_CODE": "WIFI_RAM_CODE_8163",
}

CONNECTIVITY_HELPERS = {
    "sbin/wmt_configure": (
        428704, "cb14e315e7dbacac50ed1d6bab699d97d82cc2df54c3f2a920ffdd15c6eaf58b",
    ),
    "sbin/wmt_responder": (
        428796, "e20bdaf559165077ff8211c64ed38a10ecee1006641e94302cf14d3be397c350",
    ),
    "sbin/wmt_bt_on": (
        424540, "4365c1b1046bf2ce1045a3fbd4578ee21d8f1a9900a01cb0cde9cea478821d82",
    ),
}


def fail(message: str) -> None:
    raise SystemExit("ERROR: " + message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")


def manifest_schema(manifest: dict[str, object]) -> int:
    schema_version = manifest.get("schema_version", 1)
    if type(schema_version) is not int or schema_version not in (1, 2):
        fail(f"unsupported manifest schema version: {schema_version!r}")
    return schema_version


def align(value: int) -> int:
    return (value + PAGE - 1) & ~(PAGE - 1)


def android_id(kernel: bytes, ramdisk: bytes, second: bytes, dt: bytes) -> bytes:
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        digest.update(blob)
        digest.update(struct.pack("<I", len(blob)))
    if dt:
        digest.update(dt)
        digest.update(struct.pack("<I", len(dt)))
    return digest.digest().ljust(32, b"\0")


@dataclass(frozen=True)
class Entry:
    name: str
    mode: int
    uid: int
    gid: int
    mtime: int
    data: bytes


def parse_newc(data: bytes) -> dict[str, Entry]:
    entries: dict[str, Entry] = {}
    offset = 0
    trailer = False
    while offset + 110 <= len(data):
        header = data[offset:offset + 110]
        if header[:6] != b"070701":
            if trailer and not any(data[offset:]):
                break
            fail(f"invalid newc magic at {offset:#x}")
        try:
            values = [int(header[6 + index * 8:14 + index * 8], 16) for index in range(13)]
        except ValueError:
            fail(f"invalid newc header at {offset:#x}")
        mode, uid, gid, mtime = values[1], values[2], values[3], values[5]
        size, namesize = values[6], values[11]
        offset += 110
        name_blob = data[offset:offset + namesize]
        if len(name_blob) != namesize or not name_blob.endswith(b"\0"):
            fail("truncated newc filename")
        try:
            name = name_blob[:-1].decode("utf-8")
        except UnicodeDecodeError:
            fail("non-UTF-8 newc filename")
        offset = (offset + namesize + 3) & ~3
        payload = data[offset:offset + size]
        if len(payload) != size:
            fail(f"truncated newc payload for {name}")
        offset = (offset + size + 3) & ~3
        if name == "TRAILER!!!":
            if trailer:
                fail("duplicate newc trailer")
            trailer = True
            continue
        if trailer:
            fail("newc entry follows trailer")
        normalized = name[2:] if name.startswith("./") else name
        components = normalized.split("/")
        if (
            not normalized
            or normalized.startswith("/")
            or "\0" in normalized
            or any(component in ("", ".", "..") for component in components)
        ):
            fail(f"unsafe initramfs path {name!r}")
        if normalized in entries:
            fail(f"duplicate initramfs path {normalized}")
        entries[normalized] = Entry(normalized, mode, uid, gid, mtime, payload)
    if not trailer:
        fail("newc trailer missing")
    if any(data[offset:]):
        fail("nonzero data follows newc trailer")
    return entries


def elf_info(
    data: bytes,
) -> tuple[int, int, int | None, str | None, tuple[str, ...], bool] | None:
    if data[:4] != b"\x7fELF":
        return None
    if len(data) < 20:
        fail("truncated ELF member")
    elf_class = data[4]
    if data[5] != 1:
        fail("non-little-endian ELF member")
    machine = struct.unpack_from("<H", data, 18)[0]
    if elf_class != 1:
        return elf_class, machine, None, None, (), False
    if len(data) < 52:
        fail("truncated ELF32 member")
    phoff, shoff = struct.unpack_from("<II", data, 28)
    flags = struct.unpack_from("<I", data, 36)[0]
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 42)
    interpreter = None
    has_dynamic = False
    for index in range(phnum):
        start = phoff + index * phentsize
        if start + 32 > len(data):
            fail("truncated ELF program headers")
        kind, file_offset = struct.unpack_from("<II", data, start)
        file_size = struct.unpack_from("<I", data, start + 16)[0]
        if kind == 2:
            has_dynamic = True
        if kind == 3:
            raw_interpreter = data[file_offset:file_offset + file_size]
            if len(raw_interpreter) != file_size:
                fail("truncated ELF interpreter")
            try:
                interpreter = raw_interpreter.rstrip(b"\0").decode("ascii")
            except UnicodeDecodeError:
                fail("non-ASCII ELF interpreter")

    sections: list[tuple[int, int, int, int, int]] = []
    for index in range(shnum):
        start = shoff + index * shentsize
        if start + 40 > len(data):
            fail("truncated ELF section headers")
        section_type = struct.unpack_from("<I", data, start + 4)[0]
        file_offset, size, link = struct.unpack_from("<III", data, start + 16)
        entry_size = struct.unpack_from("<I", data, start + 36)[0]
        sections.append((section_type, file_offset, size, link, entry_size))

    needed: list[str] = []
    for section_type, file_offset, size, link, entry_size in sections:
        if section_type != 6:
            continue
        if link >= len(sections):
            fail("ELF dynamic section has invalid string-table link")
        _str_type, str_offset, str_size, _str_link, _str_entry = sections[link]
        strings = data[str_offset:str_offset + str_size]
        dynamic_data = data[file_offset:file_offset + size]
        if len(strings) != str_size or len(dynamic_data) != size:
            fail("truncated ELF dynamic or string-table section")
        if entry_size not in (0, 8):
            fail("unexpected ELF32 dynamic entry size")
        for offset in range(0, len(dynamic_data) - 7, 8):
            tag, value = struct.unpack_from("<II", dynamic_data, offset)
            if tag == 0:
                break
            if tag != 1:
                continue
            if value >= len(strings):
                fail("ELF DT_NEEDED string lies outside its table")
            end = strings.find(b"\0", value)
            if end < 0:
                fail("unterminated ELF DT_NEEDED string")
            try:
                needed.append(strings[value:end].decode("ascii"))
            except UnicodeDecodeError:
                fail("non-ASCII ELF DT_NEEDED string")
    return elf_class, machine, flags, interpreter, tuple(needed), has_dynamic


def require_member(entries: dict[str, Entry], name: str, expected_hash: str,
                   permissions: int) -> Entry:
    if name not in entries:
        fail(f"initramfs lacks {name}")
    entry = entries[name]
    if not stat.S_ISREG(entry.mode) or stat.S_IMODE(entry.mode) != permissions:
        fail(f"wrong mode/type for {name}: {entry.mode:#o}")
    if sha256(entry.data) != expected_hash:
        fail(f"hash mismatch for initramfs member {name}")
    return entry


def resolve_relative_symlink(name: str, target: str) -> str:
    components = target.split("/")
    if (
        not target
        or target.startswith("/")
        or "\0" in target
        or any(component in ("", ".") for component in components)
    ):
        fail(f"unsafe initramfs symlink: {name} -> {target!r}")
    parts = list(PurePosixPath(name).parent.parts)
    if parts == ["."]:
        parts = []
    for component in components:
        if component == "..":
            if not parts:
                fail(f"initramfs symlink escapes archive root: {name} -> {target}")
            parts.pop()
        else:
            parts.append(component)
    resolved = "/".join(parts)
    if not resolved:
        fail(f"initramfs symlink resolves to archive root: {name} -> {target}")
    return resolved


def validate_archive_tree(entries: dict[str, Entry]) -> None:
    for name in entries:
        parts = PurePosixPath(name).parts
        for count in range(1, len(parts)):
            parent = "/".join(parts[:count])
            entry = entries.get(parent)
            if entry is None or not stat.S_ISDIR(entry.mode):
                fail(f"initramfs member {name} has a missing or non-directory parent {parent}")


def validate_symlinks(entries: dict[str, Entry]) -> None:
    for name, entry in entries.items():
        if not stat.S_ISLNK(entry.mode):
            continue
        current = name
        seen: set[str] = set()
        while stat.S_ISLNK(entries[current].mode):
            if current in seen:
                fail(f"initramfs symlink loop includes {current}")
            seen.add(current)
            try:
                target = entries[current].data.decode("utf-8")
            except UnicodeDecodeError:
                fail(f"non-UTF-8 initramfs symlink target for {current}")
            current = resolve_relative_symlink(current, target)
            if current not in entries:
                fail(f"dangling initramfs symlink: {name} -> {current}")
        target_entry = entries[current]
        if not (stat.S_ISREG(target_entry.mode) or stat.S_ISDIR(target_entry.mode)):
            fail(f"initramfs symlink has unsupported target type: {name} -> {current}")


def validate_no_connectivity_autostart(entries: dict[str, Entry]) -> None:
    if "init.connectivity.rc" in entries:
        fail("auto-starting init.connectivity.rc entered the initramfs")
    forbidden_launches = (
        b"wmt_loader", b"wmt_launcher", b"wmt_configure", b"wmt_responder", b"wmt_bt_on",
    )
    forbidden_wifi_writes = (
        b"> /dev/wmtWifi", b">/dev/wmtWifi", b"tee /dev/wmtWifi", b"of=/dev/wmtWifi",
    )
    active_controls = sorted(
        name for name in entries if name.endswith(".rc") or name == "libreecho-init"
    )
    for name in active_controls:
        control = entries[name].data
        for forbidden in forbidden_launches + forbidden_wifi_writes:
            if forbidden in control:
                fail(f"active recovery control {name} contains {forbidden!r}")
        for line in control.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[:2] == [b"write", b"/dev/wmtWifi"]:
                fail(f"active recovery control {name} activates Wi-Fi through Android init")


def validate_connectivity(entries: dict[str, Entry], manifest: dict[str, object],
                          schema_version: int) -> bool:
    record = manifest.get("connectivity", {"enabled": False})
    if not isinstance(record, dict) or not isinstance(record.get("enabled"), bool):
        fail("connectivity manifest record is malformed")
    bundle_names = set(CONNECTIVITY_FILES) | set(CONNECTIVITY_HELPERS) | set(CONNECTIVITY_SYMLINKS)
    if not record["enabled"]:
        unexpected = sorted(name for name in bundle_names if name in entries)
        if unexpected:
            fail(f"connectivity bundle is disabled but members are present: {unexpected}")
        if schema_version == 2:
            expected_disabled = {
                "id": CONNECTIVITY_BUNDLE_ID,
                "enabled": False,
                "activation": "manual-gates-only",
                "autostart": False,
                "files": {},
                "helpers": {},
                "symlinks": {},
            }
            if record != expected_disabled:
                fail("disabled connectivity manifest record changed")
        return False

    if schema_version != 2:
        fail("enabled connectivity bundle requires manifest schema 2")
    if record.get("id") != CONNECTIVITY_BUNDLE_ID:
        fail("connectivity bundle identity changed")
    if record.get("activation") != "manual-gates-only":
        fail("connectivity activation policy changed")
    if record.get("autostart") is not False:
        fail("connectivity autostart must remain disabled")
    expected_payload_bytes = sum(
        expected_size for _mode, expected_size, _expected_hash, _needed
        in CONNECTIVITY_FILES.values()
    ) + sum(expected_size for expected_size, _expected_hash in CONNECTIVITY_HELPERS.values())
    if record.get("stock_file_count") != len(CONNECTIVITY_FILES):
        fail("connectivity stock-file count changed")
    if record.get("helper_count") != len(CONNECTIVITY_HELPERS):
        fail("connectivity helper count changed")
    if record.get("payload_bytes") != expected_payload_bytes:
        fail("connectivity payload byte count changed")
    if record.get("provenance") != {
        "stock_system_a_sha256": CONNECTIVITY_STOCK_SYSTEM_SHA256,
        "evidence_manifest_sha256": CONNECTIVITY_EVIDENCE_MANIFEST_SHA256,
    }:
        fail("connectivity provenance changed")
    stock_root = record.get("stock_root")
    if not isinstance(stock_root, str) or not Path(stock_root).is_absolute():
        fail("connectivity stock-root provenance is not absolute")
    if record.get("reference_files_not_copied") != CONNECTIVITY_REFERENCE_FILES:
        fail("connectivity reference-file manifest mismatch")
    file_records = record.get("files")
    if not isinstance(file_records, dict) or set(file_records) != set(CONNECTIVITY_FILES):
        fail("connectivity stock-file manifest is incomplete")
    library_providers = {
        PurePosixPath(name).name: name
        for name in CONNECTIVITY_FILES if name.startswith("system/lib/")
    }
    for name, (mode, expected_size, expected_hash, needed) in CONNECTIVITY_FILES.items():
        entry = require_member(entries, name, expected_hash, mode)
        if len(entry.data) != expected_size:
            fail(f"connectivity member size mismatch for {name}")
        source = (
            "system/vendor/firmware/" + PurePosixPath(name).name
            if name.startswith("lib/firmware/") else name
        )
        expected_record: dict[str, object] = {
            "source": source,
            "sha256": expected_hash,
            "size": expected_size,
            "mode": f"{mode:04o}",
        }
        info = elf_info(entry.data)
        if needed is None:
            if info is not None:
                fail(f"connectivity firmware unexpectedly contains ELF: {name}")
        else:
            expected_info = (1, 40, 0x05000200, "/system/bin/linker", needed, True)
            if info != expected_info:
                fail(f"stock connectivity ELF contract mismatch for {name}: {info}")
            expected_record["elf"] = {
                "class": 1,
                "machine": 40,
                "flags": "0x05000200",
                "interpreter": "/system/bin/linker",
                "needed": list(needed),
                "dynamic": True,
            }
            for dependency in needed:
                if dependency not in library_providers:
                    fail(f"no staged provider for {name} dependency {dependency}")
        if file_records.get(name) != expected_record:
            fail(f"connectivity manifest record mismatch for {name}")

    helper_records = record.get("helpers")
    if not isinstance(helper_records, dict) or set(helper_records) != set(CONNECTIVITY_HELPERS):
        fail("connectivity helper manifest is incomplete")
    for name, (expected_size, expected_hash) in CONNECTIVITY_HELPERS.items():
        entry = require_member(entries, name, expected_hash, 0o755)
        if len(entry.data) != expected_size:
            fail(f"connectivity helper size mismatch for {name}")
        info = elf_info(entry.data)
        if info != (1, 40, 0x05000400, None, (), False):
            fail(f"connectivity helper is not static ARM32 hard-float: {name}: {info}")
        if helper_records.get(name) != {
            "sha256": expected_hash,
            "size": expected_size,
            "mode": "0755",
            "elf": {
                "class": 1,
                "machine": 40,
                "flags": "0x05000400",
                "interpreter": None,
                "needed": [],
                "dynamic": False,
            },
        }:
            fail(f"connectivity helper manifest record mismatch for {name}")

    if record.get("symlinks") != CONNECTIVITY_SYMLINKS:
        fail("connectivity symlink manifest mismatch")
    for name, target in CONNECTIVITY_SYMLINKS.items():
        entry = entries.get(name)
        if entry is None or not stat.S_ISLNK(entry.mode) or entry.data != target.encode():
            fail(f"connectivity symlink contract mismatch for {name}")
        resolved = resolve_relative_symlink(name, target)
        if resolved not in entries:
            fail(f"connectivity symlink dangles: {name} -> {target}")

    patch_addresses = {
        "lib/firmware/ROMv2_lm_patch_1_0_hdr.bin": bytes((0x00, 0x22, 0x00, 0x06)),
        "lib/firmware/ROMv2_lm_patch_1_1_hdr.bin": bytes((0x00, 0x21, 0x00, 0x0E)),
    }
    for name, expected_address in patch_addresses.items():
        if entries[name].data[23:27] != expected_address:
            fail(f"stock patch metadata changed for {name}")

    return True


def validate_initramfs(ramdisk: bytes, manifest: dict[str, object],
                       schema_version: int) -> bool:
    if ramdisk[:4] != b"\x1f\x8b\x08\x00":
        fail("ramdisk gzip header is not deterministic")
    try:
        cpio = gzip.decompress(ramdisk)
    except gzip.BadGzipFile as exc:
        fail(f"ramdisk gzip is invalid: {exc}")
    entries = parse_newc(cpio)
    validate_archive_tree(entries)
    validate_symlinks(entries)
    validate_no_connectivity_autostart(entries)
    if sha256(cpio) != manifest["initramfs"]["cpio_sha256"]:
        fail("manifest cpio hash mismatch")
    if any(entry.uid or entry.gid or entry.mtime for entry in entries.values()):
        fail("initramfs ownership or mtime is not normalized")

    init = require_member(entries, "init", INIT_SHA256, 0o750)
    adbd = require_member(entries, "sbin/adbd", ADBD_SHA256, 0o750)
    busybox = require_member(entries, "bin/busybox", BUSYBOX_SHA256, 0o755)
    loader = require_member(entries, "lib/ld-musl-armhf.so.1", LOADER_SHA256, 0o755)
    for name, member, expected_interpreter in (
        ("init", init, None),
        ("sbin/adbd", adbd, None),
        ("bin/busybox", busybox, "/lib/ld-musl-armhf.so.1"),
        ("lib/ld-musl-armhf.so.1", loader, None),
    ):
        info = elf_info(member.data)
        if info is None or info[:2] != (1, 40) or info[3] != expected_interpreter:
            fail(f"ELF contract mismatch for {name}: {info}")
    if b"libc.musl-armv7.so.1\0" not in busybox.data:
        fail("BusyBox musl dependency is missing")

    symlinks = {
        "lib/libc.musl-armv7.so.1": b"ld-musl-armhf.so.1",
        "sbin/sh": b"../bin/busybox",
        "sbin/ueventd": b"../init",
        "sbin/watchdogd": b"../init",
        "system/bin/sh": b"../../bin/busybox",
    }
    for name, target in symlinks.items():
        entry = entries.get(name)
        if entry is None or not stat.S_ISLNK(entry.mode) or entry.data != target:
            fail(f"symlink contract mismatch for {name}")

    required_applets = (
        "cat", "dd", "dmesg", "hexdump", "ifconfig", "insmod", "ip", "ls",
        "mknod", "mount", "rmmod", "sh", "stat", "sync", "udhcpc",
    )
    for applet in required_applets:
        entry = entries.get("bin/" + applet)
        if entry is None or not stat.S_ISLNK(entry.mode) or entry.data != b"busybox":
            fail(f"BusyBox applet link is missing or unsafe: {applet}")
    applets = manifest.get("busybox_applets", {})
    if applets.get("count", 0) < 250 or not set(required_applets).issubset(applets.get("names", [])):
        fail("BusyBox applet manifest is incomplete")

    overlay_dir = Path(__file__).resolve().parent / "initramfs"
    overlay_manifest = manifest.get("overlay", {})
    verified_overlay: dict[str, Entry] = {}
    for name, mode in OVERLAY_FILES.items():
        expected = read(overlay_dir / name)
        entry = require_member(entries, name, sha256(expected), mode)
        record = overlay_manifest.get(name, {})
        if record != {"sha256": sha256(expected), "size": len(expected), "mode": f"{mode:04o}"}:
            fail(f"overlay manifest mismatch for {name}")
        verified_overlay[name] = entry

    control = verified_overlay["libreecho-init"]
    for marker in (
        b"FASTBOOT_PLEASE", b"/tmp/runme", b"functionfs", b"/dev/stpwmt", b"/dev/stpbt",
        b"PARTNAME=expdb", b"/sys/class/block/mmcblk0p7", b"20480", b"bs=15 count=1",
        b"stat -c '%t:%T'",
    ):
        if marker not in control.data:
            fail(f"libreecho-init lacks {marker!r}")
    for forbidden in (b"/proc/hps/enabled", b"scaling_governor", b"cpuidle"):
        if forbidden in control.data:
            fail(f"libreecho-init contains forbidden policy override {forbidden!r}")
    properties = verified_overlay["default.prop"]
    for setting in (b"ro.boot.selinux=permissive", b"ro.secure=0", b"ro.debuggable=1", b"ro.adb.secure=0"):
        if setting not in properties.data.splitlines():
            fail(f"root-ADB property contract lacks {setting!r}")
    if any(name.startswith("res/") or name in {"sbin/recovery", "sbin/multi_init"} for name in entries):
        fail("unneeded stock recovery workload remains in initramfs")
    for name, entry in entries.items():
        info = elf_info(entry.data)
        if info is not None and info[:2] != (1, 40):
            fail(f"non-ARM32 ELF member {name}: {info[:2]}")
    return validate_connectivity(entries, manifest, schema_version)


def system_map_physical_end(path: Path) -> int:
    symbols: dict[str, int] = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 3:
            try:
                symbols.setdefault(fields[2], int(fields[0], 16))
            except ValueError:
                pass
    if "_text" not in symbols or "_end" not in symbols:
        fail("System.map lacks _text or _end")
    return KERNEL_ADDR + symbols["_end"] - symbols["_text"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-boot", type=Path, required=True)
    parser.add_argument("--zimage", type=Path, required=True)
    parser.add_argument("--system-map", type=Path, required=True)
    parser.add_argument("--expected-system-map-sha256", default=SYSTEM_MAP_SHA256)
    parser.add_argument("--ramdisk", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--boot-image", type=Path, required=True)
    parser.add_argument("--expected-boot-sha256", required=True)
    parser.add_argument("--expected-zimage-sha256", default=ZIMAGE_SHA256)
    parser.add_argument("--expected-dtb-sha256")
    parser.add_argument(
        "--expected-connectivity-bundle",
        choices=("none", CONNECTIVITY_BUNDLE_ID),
        default="none",
        help="require the initramfs to contain exactly this opt-in connectivity bundle",
    )
    args = parser.parse_args()

    source, zimage, system_map, ramdisk, boot = map(
        read, (args.source_boot, args.zimage, args.system_map, args.ramdisk, args.boot_image)
    )
    manifest = json.loads(args.manifest.read_text())
    schema_version = manifest_schema(manifest)
    if sha256(source) != SOURCE_SHA256:
        fail("source boot envelope hash mismatch")
    if sha256(zimage) != args.expected_zimage_sha256:
        fail("zImage hash mismatch")
    if sha256(system_map) != args.expected_system_map_sha256:
        fail("System.map hash mismatch")
    if manifest["inputs"].get("system_map", {}).get("sha256") != args.expected_system_map_sha256:
        fail("manifest System.map identity mismatch")
    if sha256(ramdisk) != manifest["initramfs"]["gzip_sha256"]:
        fail("ramdisk hash differs from manifest")
    if sha256(boot) != args.expected_boot_sha256 or manifest["output"]["sha256"] != args.expected_boot_sha256:
        fail("boot-image hash mismatch")
    if manifest.get("status") != "PREPARED_NOT_FLASHED":
        fail("manifest deployment status changed")

    if len(boot) != IMAGE_SIZE or boot[:8] != ANDROID_MAGIC:
        fail("boot image is not the 16 MiB Android v0 envelope")
    source_fields = struct.unpack_from("<10I", source, 8)
    fields = struct.unpack_from("<10I", boot, 8)
    kernel_size, kernel_addr, ramdisk_size, ramdisk_addr = fields[:4]
    second_size, second_addr, tags_addr, page_size, dt_size, unused = fields[4:]
    if (kernel_addr, ramdisk_addr, second_size, second_addr, tags_addr, page_size, dt_size, unused) != (
        KERNEL_ADDR, RAMDISK_ADDR, 0, source_fields[5], TAGS_ADDR, PAGE, 0, source_fields[9]
    ):
        fail("Android header address/geometry contract mismatch")
    if not boot[64:576].startswith(b"bootopt=64S3,32N2,32N2"):
        fail("bootopt no longer selects the proven 32-bit path")
    source_header = bytearray(source[:PAGE])
    output_header = bytearray(boot[:PAGE])
    for start, end in ((8, 12), (16, 24), (576, 608)):
        source_header[start:end] = b"\0" * (end - start)
        output_header[start:end] = b"\0" * (end - start)
    if source_header != output_header:
        fail("Android header changed outside allowed fields")

    kernel = boot[PAGE:PAGE + kernel_size]
    source_kernel = source[PAGE:PAGE + source_fields[0]]
    if kernel[:4] != MKIMG_MAGIC or kernel[8:14] != b"KERNEL":
        fail("MediaTek KERNEL header missing")
    source_mkimg, output_mkimg = bytearray(source_kernel[:MKIMG_SIZE]), bytearray(kernel[:MKIMG_SIZE])
    source_mkimg[4:8] = output_mkimg[4:8] = b"\0" * 4
    if source_mkimg != output_mkimg:
        fail("MediaTek KERNEL header changed outside payload size")
    payload_size = struct.unpack_from("<I", kernel, 4)[0]
    if kernel_size != MKIMG_SIZE + payload_size:
        fail("Android kernel size disagrees with the MediaTek payload size")
    payload = kernel[MKIMG_SIZE:MKIMG_SIZE + payload_size]
    if payload[:len(zimage)] != zimage:
        fail("zImage is not byte-identical inside MediaTek payload")
    if struct.unpack_from("<I", zimage, 0x24)[0] != ZIMAGE_MAGIC:
        fail("zImage magic mismatch")
    if struct.unpack_from("<II", zimage, 0x28) != (0, len(zimage)):
        fail("zImage range fields mismatch")
    dtb = payload[len(zimage):]
    if len(dtb) != DTB_SIZE or dtb[:4] != FDT_MAGIC or struct.unpack_from(">I", dtb, 4)[0] != DTB_SIZE:
        fail("padded appended DTB contract mismatch")
    stock_dtb = manifest["inputs"]["dtb_origin"] == "stock-envelope-extraction"
    expected_dtb = STOCK_DTB_SHA256 if stock_dtb else args.expected_dtb_sha256
    if expected_dtb is None:
        fail("--expected-dtb-sha256 is required for a supplied DTB")
    raw_size = manifest["inputs"]["dtb_raw_size"]
    if not isinstance(raw_size, int) or not 8 <= raw_size <= DTB_SIZE:
        fail("manifest raw DTB size is invalid")
    raw = bytearray(dtb[:raw_size])
    struct.pack_into(">I", raw, 4, raw_size)
    if manifest["inputs"]["dtb_raw_sha256"] != expected_dtb or sha256(bytes(raw)) != expected_dtb:
        fail("raw EVT DTB identity mismatch")
    if any(dtb[raw_size:]):
        fail("EVT DTB padding is nonzero")
    if stock_dtb and sha256(dtb) != PADDED_STOCK_DTB_SHA256:
        fail("stock EVT padded-DTB identity mismatch")

    ramdisk_offset = align(PAGE + kernel_size)
    if manifest["package"]["android"]["ramdisk_file_offset"] != f"0x{ramdisk_offset:x}":
        fail("manifest ramdisk file offset mismatch")
    if boot[ramdisk_offset:ramdisk_offset + ramdisk_size] != ramdisk:
        fail("ramdisk is not byte-identical inside boot image")
    kernel_padding = boot[PAGE + kernel_size:ramdisk_offset]
    ramdisk_end_file = ramdisk_offset + ramdisk_size
    trailing = boot[align(ramdisk_end_file):]
    if any(kernel_padding) or any(boot[ramdisk_end_file:align(ramdisk_end_file)]) or any(trailing):
        fail("section or trailing padding is nonzero")
    if boot[576:608] != android_id(kernel, ramdisk, b"", b""):
        fail("Android v0 ID mismatch")

    loaded_end = KERNEL_ADDR + payload_size
    runtime_end = system_map_physical_end(args.system_map)
    ramdisk_end = RAMDISK_ADDR + ramdisk_size
    if not (
        loaded_end < runtime_end <= ATF_START < ATF_END <= RAMDISK_ADDR <
        ramdisk_end <= RAMDISK_END_LIMIT < TAGS_ADDR
    ):
        fail("physical boot envelope overlaps or is out of order")

    connectivity_enabled = validate_initramfs(ramdisk, manifest, schema_version)
    expected_connectivity = args.expected_connectivity_bundle != "none"
    if connectivity_enabled != expected_connectivity:
        actual = CONNECTIVITY_BUNDLE_ID if connectivity_enabled else "none"
        fail(
            "connectivity bundle expectation mismatch: "
            f"expected={args.expected_connectivity_bundle} actual={actual}"
        )
    print(
        "arm32_recovery_image_contract=PASS android_v0=yes mtk_wrapper=yes "
        "zimage=yes evt_dtb=yes initramfs_arm32=yes fastboot_marker=yes "
        "root_adb_staged=yes runme=yes memory_disjoint=yes "
        f"connectivity_bundle={'yes' if connectivity_enabled else 'no'} "
        "activation=manual-only status=PREPARED_NOT_FLASHED"
    )


if __name__ == "__main__":
    main()
