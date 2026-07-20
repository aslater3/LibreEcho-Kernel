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
from pathlib import Path


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


def fail(message: str) -> None:
    raise SystemExit("ERROR: " + message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")


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
        name = name_blob[:-1].decode("utf-8")
        offset = (offset + namesize + 3) & ~3
        payload = data[offset:offset + size]
        if len(payload) != size:
            fail(f"truncated newc payload for {name}")
        offset = (offset + size + 3) & ~3
        if name == "TRAILER!!!":
            trailer = True
            continue
        if trailer:
            fail("newc entry follows trailer")
        normalized = name[2:] if name.startswith("./") else name
        if not normalized or normalized.startswith("/") or ".." in Path(normalized).parts:
            fail(f"unsafe initramfs path {name!r}")
        if normalized in entries:
            fail(f"duplicate initramfs path {normalized}")
        entries[normalized] = Entry(normalized, mode, uid, gid, mtime, payload)
    if not trailer:
        fail("newc trailer missing")
    return entries


def elf_info(data: bytes) -> tuple[int, int, bytes | None] | None:
    if data[:4] != b"\x7fELF":
        return None
    if len(data) < 52:
        fail("truncated ELF member")
    byte_order = "<" if data[5] == 1 else ">"
    elf_class = data[4]
    machine = struct.unpack_from(byte_order + "H", data, 18)[0]
    if elf_class != 1:
        return elf_class, machine, None
    phoff = struct.unpack_from(byte_order + "I", data, 28)[0]
    phentsize, phnum = struct.unpack_from(byte_order + "HH", data, 42)
    interpreter = None
    for index in range(phnum):
        start = phoff + index * phentsize
        if start + 32 > len(data):
            fail("truncated ELF program headers")
        kind, file_offset = struct.unpack_from(byte_order + "II", data, start)
        file_size = struct.unpack_from(byte_order + "I", data, start + 16)[0]
        if kind == 3:
            interpreter = data[file_offset:file_offset + file_size].rstrip(b"\0")
    return elf_class, machine, interpreter


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


def validate_initramfs(ramdisk: bytes, manifest: dict[str, object]) -> None:
    if ramdisk[:4] != b"\x1f\x8b\x08\x00":
        fail("ramdisk gzip header is not deterministic")
    try:
        cpio = gzip.decompress(ramdisk)
    except gzip.BadGzipFile as exc:
        fail(f"ramdisk gzip is invalid: {exc}")
    entries = parse_newc(cpio)
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
        ("bin/busybox", busybox, b"/lib/ld-musl-armhf.so.1"),
        ("lib/ld-musl-armhf.so.1", loader, None),
    ):
        info = elf_info(member.data)
        if info != (1, 40, expected_interpreter):
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
    args = parser.parse_args()

    source, zimage, system_map, ramdisk, boot = map(
        read, (args.source_boot, args.zimage, args.system_map, args.ramdisk, args.boot_image)
    )
    manifest = json.loads(args.manifest.read_text())
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

    validate_initramfs(ramdisk, manifest)
    print(
        "arm32_recovery_image_contract=PASS android_v0=yes mtk_wrapper=yes "
        "zimage=yes evt_dtb=yes initramfs_arm32=yes fastboot_marker=yes "
        "root_adb_staged=yes runme=yes memory_disjoint=yes status=PREPARED_NOT_FLASHED"
    )


if __name__ == "__main__":
    main()
