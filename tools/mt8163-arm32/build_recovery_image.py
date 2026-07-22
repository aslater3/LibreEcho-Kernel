#!/usr/bin/env python3
"""Build the fail-safe MT8163 ARM32 recovery ramdisk and boot image."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


ANDROID_MAGIC = b"ANDROID!"
MKIMG_MAGIC = bytes.fromhex("88168858")
FDT_MAGIC = bytes.fromhex("d00dfeed")
PAGE_SIZE = 0x800
MKIMG_SIZE = 0x200
IMAGE_SIZE = 0x1000000
KERNEL_ADDR = 0x40008000
RAMDISK_ADDR = 0x43478000
RAMDISK_END_LIMIT = 0x44000000
TAGS_ADDR = 0x48000000
ATF_START = 0x43000000
ATF_END = 0x43030000
EVT_SOURCE_OFFSET = 0x585185
EVT_RAW_SIZE = 0xC875
EVT_PADDED_SIZE = 0x10000
ZIMAGE_MAGIC = 0x016F2818

SOURCE_BOOT_SHA256 = "c0f52a3b079d214495cd3dd22f92fd85695d1b868c58b491a2edb933bc4f6d1a"
STOCK_EVT_SHA256 = "f44630ba28f503dd7503bc7cffa2ee96a319acf2f58f1456bb6f5ff23d57dee1"
BUSYBOX_SHA256 = "d4c8fd2aea01abd851c703f39b29c0de748b2751e4e1a85cae570fa53ad8f4fb"
MUSL_LOADER_SHA256 = "1063871174f1bd4f08f4d330e20b07aeb0820327ee739a4d8d1b644df842cb6b"
RECOVERY_INIT_SHA256 = "362b88597c4199aa734b3db98f2f4e7593069cd75f52b13ab5333c2599f6c5f5"
PROVEN_ZIMAGE_SHA256 = "4e144959eb0ffaee91b37d05a0f871863a74f4abb1bad0474c2fec358d5176a6"
PROVEN_SYSTEM_MAP_SHA256 = "527292112edd28e8facf2998eefe2224b08a05b193efc73634cd998e9113ba95"
CONNECTIVITY_BUNDLE_ID = "mt8163-v181-stock-v1"
CONNECTIVITY_STOCK_SYSTEM_SHA256 = "56540b3a9ac4437901a5510d9fb5e09b1a8d0cc229548f0b08bb5c22d78684fe"
CONNECTIVITY_EVIDENCE_MANIFEST_SHA256 = "d1eedd04efe0dbc78853f2b0f9357c092b4ca66242648908c0369956538441eb"
WPA_SUPPLICANT_VERSION = "2.10"

STOCK_FILES = {
    "sbin/adbd": (0o750, "1c0d14afb1ce19494ee1da935e1076f49ff57e359d348262a28bb3d56abeb930"),
    "sepolicy": (0o644, "c144b15bff55da40125055b3e8aa134d204e0877c1712f15a313bc5555e8113a"),
    "file_contexts.bin": (0o644, "1bc8fa508de455f391edabe1c44dc4cf230b7a21dab5824f29f0d36b2a6944ac"),
    "property_contexts": (0o644, "921c3c53f6279bba57a93714504b3300cc96f244e12c7dde17886b564677f9ba"),
    "service_contexts": (0o644, "3ee92dc3d98b18d0c3e338dec3743ca9591920f51059e3a8279b670127003c3e"),
    "seapp_contexts": (0o644, "a36f09a131e3b983edf41815e7a2e1afa5807b823c7c5c10bd4a97c08c5e816d"),
    "selinux_version": (0o644, "fab0d130803f8aca27b4a6ac8aea7ae55f4c8d0f36ec90a2ee32cb13aa581cbe"),
    "ueventd.rc": (0o644, "f702275ec262b58184e53d2dd3f213e1538fa75985b88f7cb6c5bbde74f88062"),
    "ueventd.mt8163.rc": (0o644, "b1d212a42d213b4b1412648e7501baf55aa3ee653236cdf10f650050e0ea325c"),
}

CONNECTIVITY_STOCK_FILES = {
    "system/bin/linker": {
        "source": "system/bin/linker", "mode": 0o755, "size": 630460,
        "sha256": "73dc93e06a9ce0a76b5353f2c282f1ac3dd0dccd0e8e7f06fc20e5433ef4a3dc",
        "needed": (),
    },
    "system/vendor/bin/wmt_loader": {
        "source": "system/vendor/bin/wmt_loader", "mode": 0o755, "size": 17992,
        "sha256": "de9ee285a09a7db5b079233f7c9129c5484ecb6701b54da45e2a29f310e74ff9",
        "needed": ("libcutils.so", "libc++.so", "libdl.so", "libc.so", "libm.so"),
    },
    "system/vendor/bin/wmt_launcher": {
        "source": "system/vendor/bin/wmt_launcher", "mode": 0o755, "size": 31448,
        "sha256": "1f34425d727ea64524c9edaeac5e6b295df7a6054703dcc79b164021560252e5",
        "needed": ("libcutils.so", "libc++.so", "libdl.so", "libc.so", "libm.so"),
    },
    "lib/firmware/ROMv2_lm_patch_1_0_hdr.bin": {
        "source": "system/vendor/firmware/ROMv2_lm_patch_1_0_hdr.bin", "mode": 0o644,
        "size": 128720,
        "sha256": "b4460117f51a43f3284594ec08d8c8861ecc0e42b17820987da03ecabdebac1e",
    },
    "lib/firmware/ROMv2_lm_patch_1_1_hdr.bin": {
        "source": "system/vendor/firmware/ROMv2_lm_patch_1_1_hdr.bin", "mode": 0o644,
        "size": 50148,
        "sha256": "10c4ed22a10b8a136bffd7ffce4d552300d76f8e593627d2a9841c3b11a5697e",
    },
    "lib/firmware/WIFI_RAM_CODE_8163": {
        "source": "system/vendor/firmware/WIFI_RAM_CODE_8163", "mode": 0o644,
        "size": 373840,
        "sha256": "9669cc9b03cfdc5e8fd4fd6e14c4c4050e8c196738ca4707eea12f14a6a8e64c",
    },
    "lib/firmware/WMT_SOC.cfg": {
        "source": "system/vendor/firmware/WMT_SOC.cfg", "mode": 0o644, "size": 119,
        "sha256": "302bd4462de99c028c04092e561c1500d65582ce42a93c4c72ccae6e2c99013d",
    },
    "system/lib/libcutils.so": {
        "source": "system/lib/libcutils.so", "mode": 0o644, "size": 104436,
        "sha256": "dcf249ceed2c84ab45454ff8fd3fa0624248b410962c4ea9e9e799610192542b",
        "needed": ("liblog.so", "libc++.so", "libdl.so", "libc.so", "libm.so"),
    },
    "system/lib/libc++.so": {
        "source": "system/lib/libc++.so", "mode": 0o644, "size": 575068,
        "sha256": "38f15c7897307e65c9b9a13174782e7b79146e453b8b80e09128aae8b6ab1df5",
        "needed": ("libdl.so", "libc.so", "libm.so"),
    },
    "system/lib/libdl.so": {
        "source": "system/lib/libdl.so", "mode": 0o644, "size": 13640,
        "sha256": "efb8d634212b215b53f8c95f2b8372e9139ee13dc74717b7d25999de97d5b1cc",
        "needed": (),
    },
    "system/lib/libc.so": {
        "source": "system/lib/libc.so", "mode": 0o644, "size": 780476,
        "sha256": "1254edac10625b1e7e123c20ea8d8f3175ad07014c9ddcca7bb3ea74db555357",
        "needed": ("libdl.so",),
    },
    "system/lib/libm.so": {
        "source": "system/lib/libm.so", "mode": 0o644, "size": 132820,
        "sha256": "3703abfae55405f1ca876cfaf5c8e41b0dafdd30d4ecec88cbd1100c5b0341ed",
        "needed": ("libc.so",),
    },
    "system/lib/liblog.so": {
        "source": "system/lib/liblog.so", "mode": 0o644, "size": 67460,
        "sha256": "84e34e101618dae346cefca70c8cd866b92e6bcdec64246a130dcd12560410c0",
        "needed": ("libc.so", "libm.so"),
    },
}

CONNECTIVITY_REFERENCE_FILES = {
    "init.connectivity.rc": (3167, "142c3f2239255dff573196daaf7da00687be9c5c54174dcbecfa309074d9d379"),
    "ueventd.mt8163.rc": (4255, "b1d212a42d213b4b1412648e7501baf55aa3ee653236cdf10f650050e0ea325c"),
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
        "wmt_config_helper", 428704,
        "2fa1c78546b3a0d35442ffa196f3eaa13b1ce4609b537332b016bc88ea663be2",
    ),
    "sbin/wmt_responder": (
        "wmt_responder", 428796,
        "e20bdaf559165077ff8211c64ed38a10ecee1006641e94302cf14d3be397c350",
    ),
    "sbin/wmt_bt_on": (
        "wmt_bt_on", 424540,
        "4365c1b1046bf2ce1045a3fbd4578ee21d8f1a9900a01cb0cde9cea478821d82",
    ),
    "sbin/wmt_stock_compat": (
        "wmt_stock_compat", 341184,
        "5be9b801153c79f85260b193c57a5ba5c4155f9fccbad47a794e9445e94d654c",
    ),
    "sbin/wmt_launcher": (
        "wmt_launcher", 428912,
        "6e65e46536bfea0b44f0887998a4d556338250d42609e13fbe6d7833a08187c3",
    ),
}

CONNECTIVITY_PATCH_ROUTES = {
    "lib/firmware/ROMv2_lm_patch_1_0_hdr.bin": (
        bytes((0x8A, 0x00)), bytes((0x22, 0x00, 0x06, 0x00)), 2,
        bytes((0x00, 0x00, 0x06, 0x00)),
    ),
    "lib/firmware/ROMv2_lm_patch_1_1_hdr.bin": (
        bytes((0x8A, 0x00)), bytes((0x21, 0x00, 0x0E, 0xF0)), 1,
        bytes((0x00, 0x00, 0x0E, 0xF0)),
    ),
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise SystemExit(f"ERROR: cannot read {path}: {exc}") from exc


def require_hash(label: str, data: bytes, expected: str) -> None:
    actual = sha256(data)
    if actual != expected:
        raise SystemExit(f"ERROR: {label} SHA-256 mismatch\nexpected={expected}\nactual={actual}")


def align(value: int, size: int = PAGE_SIZE) -> int:
    return (value + size - 1) & ~(size - 1)


def parse_int(value: str) -> int:
    return int(value, 0)


def android_id(kernel: bytes, ramdisk: bytes, second: bytes, dt: bytes) -> bytes:
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        digest.update(blob)
        digest.update(struct.pack("<I", len(blob)))
    if dt:
        digest.update(dt)
        digest.update(struct.pack("<I", len(dt)))
    return digest.digest().ljust(32, b"\0")


def elf_identity(path: Path) -> tuple[int, int] | None:
    data = read(path)
    if data[:4] != b"\x7fELF":
        return None
    if len(data) < 20:
        raise SystemExit(f"ERROR: truncated ELF file: {path}")
    byte_order = "<" if data[5] == 1 else ">"
    return data[4], struct.unpack_from(byte_order + "H", data, 18)[0]


def readelf_contract(path: Path) -> tuple[int, str | None, tuple[str, ...], bool]:
    try:
        output = subprocess.run(
            ["readelf", "-h", "-l", "-d", str(path)], check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C"},
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"ERROR: cannot inspect ELF contract for {path}: {exc}") from exc
    if "Class:                             ELF32" not in output:
        raise SystemExit(f"ERROR: {path} is not ELF32")
    if "Data:                              2's complement, little endian" not in output:
        raise SystemExit(f"ERROR: {path} is not little-endian ELF")
    if "Machine:                           ARM" not in output:
        raise SystemExit(f"ERROR: {path} is not an ARM ELF")
    flags_match = re.search(r"^\s*Flags:\s+(0x[0-9a-fA-F]+)", output, re.MULTILINE)
    if flags_match is None:
        raise SystemExit(f"ERROR: readelf did not report ARM ABI flags for {path}")
    interpreter_match = re.search(r"\[Requesting program interpreter: ([^]]+)\]", output)
    interpreter = interpreter_match.group(1) if interpreter_match else None
    needed = tuple(re.findall(r"\(NEEDED\).*Shared library: \[([^]]+)\]", output))
    dynamic = re.search(r"^\s*DYNAMIC\s", output, re.MULTILINE) is not None
    return int(flags_match.group(1), 16), interpreter, needed, dynamic


def require_elf_contract(path: Path, flags: int, interpreter: str | None,
                         needed: tuple[str, ...], dynamic: bool) -> dict[str, object]:
    ident = elf_identity(path)
    if ident != (1, 40):
        raise SystemExit(f"ERROR: ELF identity mismatch for {path}: {ident}")
    actual = readelf_contract(path)
    expected = (flags, interpreter, needed, dynamic)
    if actual != expected:
        raise SystemExit(f"ERROR: ELF ABI/dependency mismatch for {path}: expected={expected!r} actual={actual!r}")
    return {
        "class": 1,
        "machine": 40,
        "flags": f"0x{flags:08x}",
        "interpreter": interpreter,
        "needed": list(needed),
        "dynamic": dynamic,
    }


def pinned_source(root: Path, relative: str, label: str) -> Path:
    relative_path = Path(relative)
    components = relative.split("/")
    if (
        not relative
        or relative_path.is_absolute()
        or any(part in ("", ".", "..") for part in components)
        or relative_path.as_posix() != relative
    ):
        raise SystemExit(f"ERROR: unsafe pinned source path for {label}: {relative!r}")
    source = root
    for part in relative_path.parts:
        source /= part
        if source.is_symlink():
            raise SystemExit(f"ERROR: symlink in pinned source path for {label}: {source}")
    if not source.is_file():
        raise SystemExit(f"ERROR: pinned source is not a regular file for {label}: {source}")
    return source


def copy_pinned(source_root: Path, stage: Path, manifest: dict[str, object]) -> None:
    copied: dict[str, object] = {}
    for relative, (mode, expected) in STOCK_FILES.items():
        source = pinned_source(source_root, relative, f"stock userspace {relative}")
        data = read(source)
        require_hash(f"stock userspace {relative}", data, expected)
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
        copied[relative] = {"sha256": expected, "size": len(data), "mode": f"{mode:04o}"}
    manifest["stock_userspace"] = copied


def add_connectivity_bundle(source_root: Path, stage: Path,
                            helpers: dict[str, Path], manifest: dict[str, object]) -> None:
    references: dict[str, object] = {}
    for relative, (expected_size, expected_hash) in CONNECTIVITY_REFERENCE_FILES.items():
        reference = pinned_source(source_root, relative, f"connectivity reference {relative}")
        data = read(reference)
        if len(data) != expected_size:
            raise SystemExit(
                f"ERROR: connectivity reference {relative} size mismatch: "
                f"expected={expected_size} actual={len(data)}"
            )
        require_hash(f"connectivity reference {relative}", data, expected_hash)
        references[relative] = {"sha256": expected_hash, "size": expected_size}

    copied: dict[str, object] = {}
    for target_name, specification in CONNECTIVITY_STOCK_FILES.items():
        source_name = str(specification["source"])
        expected_size = int(specification["size"])
        expected_hash = str(specification["sha256"])
        mode = int(specification["mode"])
        source = pinned_source(source_root, source_name, f"connectivity asset {source_name}")
        data = read(source)
        if len(data) != expected_size:
            raise SystemExit(
                f"ERROR: connectivity asset {source_name} size mismatch: "
                f"expected={expected_size} actual={len(data)}"
            )
        require_hash(f"connectivity asset {source_name}", data, expected_hash)
        target = stage / target_name
        if target.exists() or target.is_symlink():
            raise SystemExit(f"ERROR: connectivity asset collides with {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
        record: dict[str, object] = {
            "source": source_name,
            "sha256": expected_hash,
            "size": expected_size,
            "mode": f"{mode:04o}",
        }
        if "needed" in specification:
            record["elf"] = require_elf_contract(
                target, 0x05000200, "/system/bin/linker",
                tuple(specification["needed"]), True,
            )
        copied[target_name] = record

    helper_records: dict[str, object] = {}
    for target_name, (argument_name, expected_size, expected_hash) in CONNECTIVITY_HELPERS.items():
        source = helpers[argument_name]
        if source.is_symlink() or not source.is_file():
            raise SystemExit(f"ERROR: connectivity helper is not a regular file: {source}")
        data = read(source)
        if len(data) != expected_size:
            raise SystemExit(
                f"ERROR: connectivity helper {argument_name} size mismatch: "
                f"expected={expected_size} actual={len(data)}"
            )
        require_hash(f"connectivity helper {argument_name}", data, expected_hash)
        target = stage / target_name
        if target.exists() or target.is_symlink():
            raise SystemExit(f"ERROR: connectivity helper collides with {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o755)
        helper_records[target_name] = {
            "sha256": expected_hash,
            "size": expected_size,
            "mode": "0755",
            "elf": require_elf_contract(target, 0x05000400, None, (), False),
        }

    symlink_records: dict[str, str] = {}
    for relative, link_target in CONNECTIVITY_SYMLINKS.items():
        target = stage / relative
        if target.exists() or target.is_symlink():
            raise SystemExit(f"ERROR: connectivity symlink collides with {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(link_target, target)
        try:
            target.resolve(strict=True).relative_to(stage.resolve())
        except (OSError, ValueError) as exc:
            raise SystemExit(f"ERROR: connectivity symlink escapes or dangles: {relative} -> {link_target}") from exc
        symlink_records[relative] = link_target

    patch_routing_records: dict[str, object] = {}
    for relative, (expected_header, expected_route, expected_seq,
                   expected_address) in CONNECTIVITY_PATCH_ROUTES.items():
        data = read(stage / relative)
        route = data[24:28]
        patch_count = route[0] >> 4
        download_seq = route[0] & 0x0F
        address = b"\0" + route[1:]
        if (data[22:24] != expected_header or route != expected_route or
                patch_count != len(CONNECTIVITY_PATCH_ROUTES) or
                download_seq != expected_seq or address != expected_address):
            raise SystemExit(f"ERROR: stock patch metadata changed for {relative}")
        patch_routing_records[relative] = {
            "header": expected_header.hex(),
            "route": expected_route.hex(),
            "patch_count": patch_count,
            "download_seq": download_seq,
            "address": address.hex(),
        }

    if read(stage / "ueventd.mt8163.rc") != read(source_root / "ueventd.mt8163.rc"):
        raise SystemExit("ERROR: connectivity root and recovery use different ueventd.mt8163.rc files")
    if (stage / "init.connectivity.rc").exists():
        raise SystemExit("ERROR: auto-starting init.connectivity.rc entered the recovery stage")
    manifest["connectivity"] = {
        "id": CONNECTIVITY_BUNDLE_ID,
        "enabled": True,
        "activation": "manual-gates-only",
        "autostart": False,
        "stock_file_count": len(copied),
        "helper_count": len(helper_records),
        "payload_bytes": sum(record["size"] for record in copied.values())
                         + sum(record["size"] for record in helper_records.values()),
        "provenance": {
            "stock_system_a_sha256": CONNECTIVITY_STOCK_SYSTEM_SHA256,
            "evidence_manifest_sha256": CONNECTIVITY_EVIDENCE_MANIFEST_SHA256,
        },
        "stock_root": str(source_root),
        "reference_files_not_copied": references,
        "files": copied,
        "helpers": helper_records,
        "symlinks": symlink_records,
        "patch_routing": patch_routing_records,
    }


def add_overlay(stage: Path, overlay: Path, busybox: Path, loader: Path,
                qemu_arm: str, manifest: dict[str, object]) -> None:
    directories = (
        "bin", "dev", "dev/pts", "dev/socket", "dev/usb-ffs", "dev/usb-ffs/adb",
        "etc", "etc/wifi", "lib", "lib/firmware", "proc", "sbin", "sys", "system", "system/bin", "tmp",
    )
    for directory in directories:
        target = stage / directory
        target.mkdir(parents=True, exist_ok=True)
        target.chmod(0o777 if directory == "tmp" else 0o755)

    overlay_files = {
        "default.prop": ("default.prop", 0o644),
        "init.rc": ("init.rc", 0o644),
        "init.recovery.mt8163.rc": ("init.recovery.mt8163.rc", 0o644),
        "libreecho-init": ("libreecho-init", 0o755),
        "libreecho-wifi": ("sbin/libreecho-wifi", 0o755),
        "udhcpc.script": ("etc/udhcpc.script", 0o755),
        "wpa_supplicant.conf.example": (
            "etc/wifi/wpa_supplicant.conf.example", 0o600,
        ),
    }
    overlay_manifest: dict[str, object] = {}
    for relative, (target_relative, mode) in overlay_files.items():
        data = read(overlay / relative)
        target = stage / target_relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
        overlay_manifest[relative] = {"sha256": sha256(data), "size": len(data), "mode": f"{mode:04o}"}

    # The stock ramdisk's /init is an Android ELF that is incompatible with
    # this ARM32 recovery kernel.  PID 1 must be the audited LibreEcho shell
    # control script, installed at the real runtime path (not merely staged
    # as /libreecho-init).
    init_script = read(overlay / "libreecho-init")
    require_hash("LibreEcho recovery /init", init_script, RECOVERY_INIT_SHA256)
    init_target = stage / "init"
    init_target.write_bytes(init_script)
    init_target.chmod(0o755)
    overlay_manifest["init"] = {
        "sha256": RECOVERY_INIT_SHA256,
        "size": len(init_script),
        "mode": "0755",
        "source": "libreecho-init",
    }

    busybox_data = read(busybox)
    loader_data = read(loader)
    require_hash("ARM32 BusyBox", busybox_data, BUSYBOX_SHA256)
    require_hash("ARM32 musl loader", loader_data, MUSL_LOADER_SHA256)
    (stage / "bin/busybox").write_bytes(busybox_data)
    (stage / "bin/busybox").chmod(0o755)
    (stage / "lib/ld-musl-armhf.so.1").write_bytes(loader_data)
    (stage / "lib/ld-musl-armhf.so.1").chmod(0o755)

    fixed_links = {
        "lib/libc.musl-armv7.so.1": "ld-musl-armhf.so.1",
        "sbin/sh": "../bin/busybox",
        "sbin/ueventd": "../init",
        "sbin/watchdogd": "../init",
        "system/bin/sh": "../../bin/busybox",
    }
    for relative, target in fixed_links.items():
        os.symlink(target, stage / relative)

    applet_output = subprocess.run(
        [qemu_arm, "-L", str(stage), str(stage / "bin/busybox"), "--list"],
        check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout
    applets = sorted(set(applet_output.splitlines()))
    if len(applets) < 250:
        raise SystemExit(f"ERROR: BusyBox applet inventory is unexpectedly short: {len(applets)}")
    for applet in applets:
        if not applet or "/" in applet or applet in {".", "..", "busybox"}:
            raise SystemExit(f"ERROR: unsafe BusyBox applet name {applet!r}")
        target = stage / "bin" / applet
        if target.exists() or target.is_symlink():
            raise SystemExit(f"ERROR: BusyBox applet collides with {target}")
        os.symlink("busybox", target)

    manifest["overlay"] = overlay_manifest
    manifest["busybox"] = {"sha256": BUSYBOX_SHA256, "size": len(busybox_data)}
    manifest["musl_loader"] = {"sha256": MUSL_LOADER_SHA256, "size": len(loader_data)}
    manifest["symlinks"] = fixed_links
    manifest["busybox_applets"] = {"count": len(applets), "names": applets}


def add_audio_probe(stage: Path, audio_probe: Path,
                    manifest: dict[str, object]) -> None:
    """Install the dependency-free ARM32 ALSA capability probe."""
    if audio_probe.is_symlink() or not audio_probe.is_file():
        raise SystemExit(f"ERROR: audio probe is not a regular file: {audio_probe}")
    data = read(audio_probe)
    target = stage / "sbin/audio_probe"
    if target.exists() or target.is_symlink():
        raise SystemExit(f"ERROR: audio probe collides with {target}")
    target.write_bytes(data)
    target.chmod(0o755)
    manifest["audio"] = {
        "enabled": True,
        "activation": "manual-only",
        "probe": {
            "path": str(audio_probe.resolve()),
            "sha256": sha256(data),
            "size": len(data),
            "mode": "0755",
            "elf": require_elf_contract(target, 0x05000400, None, (), False),
        },
    }


def add_audio_tools(stage: Path, tinyplay: Path, tinycap: Path, tinymix: Path,
                    manifest: dict[str, object]) -> None:
    """Install the patched static TinyALSA playback/capture/mixer utilities."""
    audio = manifest.get("audio")
    if not isinstance(audio, dict) or not audio.get("enabled"):
        raise SystemExit("ERROR: audio tools require the audio probe")

    tools: dict[str, object] = {}
    for name, source, target_name in (
        ("tinyplay", tinyplay, "sbin/tinyplay"),
        ("tinycap", tinycap, "sbin/tinycap"),
        ("tinymix", tinymix, "sbin/tinymix"),
    ):
        if source.is_symlink() or not source.is_file():
            raise SystemExit(f"ERROR: {name} is not a regular file: {source}")
        data = read(source)
        target = stage / target_name
        if target.exists() or target.is_symlink():
            raise SystemExit(f"ERROR: audio tool collides with {target}")
        target.write_bytes(data)
        target.chmod(0o755)
        tools[name] = {
            "path": str(source.resolve()),
            "sha256": sha256(data),
            "size": len(data),
            "mode": "0755",
            "elf": require_elf_contract(target, 0x05000400, None, (), False),
        }
    audio["tools"] = tools


def add_startup_audio(stage: Path, startup_audio: Path,
                      manifest: dict[str, object]) -> None:
    """Install the bounded post-init HPR confirmation clip."""
    audio = manifest.get("audio")
    if not isinstance(audio, dict) or not audio.get("enabled"):
        raise SystemExit("ERROR: startup audio requires the audio tools")
    if startup_audio.is_symlink() or not startup_audio.is_file():
        raise SystemExit(f"ERROR: startup audio is not a regular file: {startup_audio}")
    data = read(startup_audio)
    try:
        import io
        import wave
        with wave.open(io.BytesIO(data), "rb") as wav:
            audio_format = {
                "channels": wav.getnchannels(),
                "sample_rate": wav.getframerate(),
                "sample_width_bits": wav.getsampwidth() * 8,
                "compression": wav.getcomptype(),
            }
    except (EOFError, wave.Error) as exc:
        raise SystemExit(f"ERROR: startup audio is not a readable WAV: {startup_audio}") from exc
    if audio_format != {
        "channels": 2,
        "sample_rate": 48000,
        "sample_width_bits": 16,
        "compression": "NONE",
    }:
        raise SystemExit(f"ERROR: startup audio format is not stereo 48kHz PCM16: {audio_format}")
    target = stage / "etc/audio/windows95-startup.wav"
    if target.exists() or target.is_symlink():
        raise SystemExit(f"ERROR: startup audio collides with {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(0o644)
    audio["activation"] = "automatic-after-successful-init"
    audio["startup_playback"] = {
        "path": str(startup_audio.resolve()),
        "sha256": sha256(data),
        "size": len(data),
        "mode": "0644",
        "format": audio_format,
        "route": "hpr-only",
        "pcm_volume": "103/103",
        "pcm_db": "-12.0",
        "hp_driver_gain": "6/6",
        "lineout_dac_switches": "off",
        "playback_device": "0:23",
        "plays_once": True,
    }


def add_network_bundle(stage: Path, wpa_supplicant: Path, wifi_config: Path,
                       manifest: dict[str, object]) -> None:
    """Add the verified static WPA client and a build-local Wi-Fi profile."""
    if wpa_supplicant.is_symlink() or not wpa_supplicant.is_file():
        raise SystemExit(f"ERROR: wpa_supplicant is not a regular file: {wpa_supplicant}")
    if wifi_config.is_symlink() or not wifi_config.is_file():
        raise SystemExit(f"ERROR: Wi-Fi profile is not a regular file: {wifi_config}")
    wpa_data = read(wpa_supplicant)
    config_data = read(wifi_config)
    target = stage / "sbin/wpa_supplicant"
    if target.exists() or target.is_symlink():
        raise SystemExit(f"ERROR: network asset collides with {target}")
    target.write_bytes(wpa_data)
    target.chmod(0o755)
    elf = require_elf_contract(target, 0x05000400, None, (), False)
    if b"wpa_supplicant v2.10" not in wpa_data:
        raise SystemExit("ERROR: static wpa_supplicant does not identify as v2.10")
    if b"CHANGE_ME" in config_data:
        raise SystemExit("ERROR: refusing to package the unconfigured Wi-Fi profile template")
    config_target = stage / "etc/wifi/wpa_supplicant.conf"
    config_target.write_bytes(config_data)
    config_target.chmod(0o600)
    manifest["network"] = {
        "enabled": True,
        "activation": "automatic-after-adb-if-profile-present",
        "wpa_supplicant": {
            "version": WPA_SUPPLICANT_VERSION,
            "sha256": sha256(wpa_data),
            "size": len(wpa_data),
            "mode": "0755",
            "elf": elf,
        },
        "wifi_profile": {
            "sha256": sha256(config_data),
            "size": len(config_data),
            "mode": "0600",
            "secret_content_not_recorded": True,
        },
        "dhcp": "busybox-udhcpc",
        "dhcp_hook": "/etc/udhcpc.script",
    }


def validate_stage(stage: Path) -> None:
    required = (
        "init", "init.rc", "libreecho-init", "bin/busybox",
        "lib/ld-musl-armhf.so.1", "lib/libc.musl-armv7.so.1",
        "sbin/adbd", "sbin/ueventd", "sbin/sh", "system/bin/sh",
        "sepolicy", "file_contexts.bin", "property_contexts",
    )
    for relative in required:
        if not (stage / relative).exists():
            raise SystemExit(f"ERROR: initramfs is missing {relative}")

    stage_root = stage.resolve()
    for path in sorted(stage.rglob("*")):
        if path.is_symlink():
            target = os.readlink(path)
            components = target.split("/")
            if (
                not target
                or target.startswith("/")
                or "\0" in target
                or any(component in ("", ".") for component in components)
            ):
                raise SystemExit(f"ERROR: unsafe initramfs symlink: {path} -> {target!r}")
            try:
                path.resolve(strict=True).relative_to(stage_root)
            except (OSError, RuntimeError, ValueError) as exc:
                raise SystemExit(f"ERROR: initramfs symlink escapes, dangles, or loops: {path}") from exc
            continue
        if not path.is_file():
            continue
        ident = elf_identity(path)
        if ident is not None and ident != (1, 40):
            raise SystemExit(f"ERROR: non-ARM32 ELF in initramfs: {path} class={ident[0]} machine={ident[1]}")

    # /init is intentionally a script.  Only the native helper is required
    # to be a static ARM32 ELF here; treating the script as an ELF was the
    # stale-builder bug that allowed the stock PID 1 back into the image.
    output = subprocess.run(
        ["readelf", "-l", str(stage / "sbin/adbd")], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout
    if "Requesting program interpreter" in output:
        raise SystemExit("ERROR: sbin/adbd is not static")
    init_script = read(stage / "init")
    if init_script != read(stage / "libreecho-init"):
        raise SystemExit("ERROR: runtime /init differs from audited libreecho-init")
    if not init_script.startswith(b"#!/bin/busybox sh\n"):
        raise SystemExit("ERROR: runtime /init is not the audited BusyBox shell script")

    busybox_program = subprocess.run(
        ["readelf", "-l", str(stage / "bin/busybox")], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout
    busybox_dynamic = subprocess.run(
        ["readelf", "-d", str(stage / "bin/busybox")], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout
    if "/lib/ld-musl-armhf.so.1" not in busybox_program:
        raise SystemExit("ERROR: BusyBox interpreter contract changed")
    if "libc.musl-armv7.so.1" not in busybox_dynamic:
        raise SystemExit("ERROR: BusyBox DT_NEEDED contract changed")

    init_script = read(stage / "libreecho-init")
    for marker in (
        b"FASTBOOT_PLEASE", b"/tmp/runme", b"functionfs", b"/dev/stpwmt", b"/dev/stpbt",
        b"PARTNAME=expdb", b"/sys/class/block/mmcblk0p7", b"20480", b"bs=15 count=1",
        b"stat -c '%t:%T'",
    ):
        if marker not in init_script:
            raise SystemExit(f"ERROR: recovery control script lacks {marker!r}")
    adbd_launches = tuple(
        line.strip() for line in init_script.splitlines()
        if line.lstrip().startswith(b"/sbin/adbd ")
    )
    if adbd_launches != (
        b"/sbin/adbd --root_seclabel=u:r:su:s0 --device_banner=device </dev/null >/tmp/adbd.log 2>&1 &",
    ):
        raise SystemExit(f"ERROR: unexpected ARM32 adbd launch contract: {adbd_launches!r}")
    for forbidden in (b"/proc/hps/enabled", b"scaling_governor", b"cpuidle"):
        if forbidden in init_script:
            raise SystemExit(f"ERROR: recovery control script contains forbidden policy override {forbidden!r}")
    properties = read(stage / "default.prop")
    for setting in (b"ro.boot.selinux=permissive", b"ro.secure=0", b"ro.debuggable=1", b"ro.adb.secure=0"):
        if setting not in properties.splitlines():
            raise SystemExit(f"ERROR: recovery property contract lacks {setting!r}")

    active_controls = sorted(
        path.relative_to(stage).as_posix()
        for path in stage.rglob("*.rc")
        if path.is_file()
    )
    active_controls.append("libreecho-init")
    forbidden_launches = (
        b"wmt_loader", b"wmt_launcher", b"wmt_configure", b"wmt_responder", b"wmt_bt_on",
    )
    forbidden_wifi_writes = (
        b"> /dev/wmtWifi", b">/dev/wmtWifi", b"tee /dev/wmtWifi", b"of=/dev/wmtWifi",
    )
    for relative in active_controls:
        control = read(stage / relative)
        forbidden = () if relative == "libreecho-init" else forbidden_launches + forbidden_wifi_writes
        for marker in forbidden:
            if marker in control:
                raise SystemExit(f"ERROR: active recovery control {relative} contains {marker!r}")
        for line in control.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[:2] == [b"write", b"/dev/wmtWifi"]:
                raise SystemExit(
                    f"ERROR: active recovery control {relative} activates Wi-Fi through Android init"
                )
    if (stage / "init.connectivity.rc").exists():
        raise SystemExit("ERROR: auto-starting init.connectivity.rc is forbidden")


def build_cpio(stage: Path, epoch: int) -> bytes:
    for path in [stage, *sorted(stage.rglob("*"))]:
        os.utime(path, (epoch, epoch), follow_symlinks=False)
    paths = sorted(
        (path.relative_to(stage) for path in stage.rglob("*")),
        key=lambda path: (len(path.parts), path.as_posix()),
    )
    names = b"".join(("./" + path.as_posix()).encode() + b"\0" for path in paths)
    result = subprocess.run(
        ["cpio", "--null", "--create", "--format=newc", "--owner=0:0", "--reproducible", "--quiet"],
        cwd=stage, input=names, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C"}, check=True,
    )
    if not result.stdout.startswith(b"070701"):
        raise SystemExit("ERROR: generated initramfs is not a newc archive")
    return result.stdout


def extract_or_read_dtb(source: bytes, supplied: Path | None, expected: str | None) -> tuple[bytes, str]:
    fields = struct.unpack_from("<10I", source, 8)
    old_kernel = source[PAGE_SIZE:PAGE_SIZE + fields[0]]
    if old_kernel[:4] != MKIMG_MAGIC:
        raise SystemExit("ERROR: source MediaTek KERNEL header missing")
    if supplied is None:
        payload_size = struct.unpack_from("<I", old_kernel, 4)[0]
        payload = old_kernel[MKIMG_SIZE:MKIMG_SIZE + payload_size]
        raw = payload[EVT_SOURCE_OFFSET:EVT_SOURCE_OFFSET + EVT_RAW_SIZE]
        require_hash("stock EVT DTB", raw, STOCK_EVT_SHA256)
        origin = "stock-envelope-extraction"
    else:
        raw = read(supplied)
        if expected is None:
            raise SystemExit("ERROR: --expected-dtb-sha256 is required with --dtb")
        require_hash("supplied DTB", raw, expected)
        origin = str(supplied.resolve())
    if raw[:4] != FDT_MAGIC or len(raw) < 8:
        raise SystemExit("ERROR: DTB magic missing")
    total = struct.unpack_from(">I", raw, 4)[0]
    if total > len(raw) or total > EVT_PADDED_SIZE:
        raise SystemExit(f"ERROR: invalid DTB totalsize {total:#x} for file size {len(raw):#x}")
    return raw[:total], origin


def padded_dtb(raw: bytes) -> bytes:
    result = bytearray(EVT_PADDED_SIZE)
    result[:len(raw)] = raw
    struct.pack_into(">I", result, 4, EVT_PADDED_SIZE)
    return bytes(result)


def system_map_end(path: Path, kernel_addr: int) -> tuple[int, dict[str, str]]:
    symbols: dict[str, int] = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 3:
            try:
                symbols.setdefault(fields[2], int(fields[0], 16))
            except ValueError:
                pass
    if "_text" not in symbols or "_end" not in symbols:
        raise SystemExit("ERROR: System.map lacks _text or _end")
    physical_end = kernel_addr + symbols["_end"] - symbols["_text"]
    return physical_end, {
        "sha256": sha256(read(path)),
        "_text": f"0x{symbols['_text']:08x}",
        "_end": f"0x{symbols['_end']:08x}",
        "physical_end": f"0x{physical_end:08x}",
    }


def package_boot(source: bytes, zimage: bytes, ramdisk: bytes, raw_dtb: bytes,
                 ramdisk_addr: int, system_map: Path | None) -> tuple[bytes, dict[str, object]]:
    if source[:8] != ANDROID_MAGIC or len(source) != IMAGE_SIZE:
        raise SystemExit("ERROR: source is not the pinned 16 MiB Android boot envelope")
    fields = list(struct.unpack_from("<10I", source, 8))
    old_kernel_size, kernel_addr = fields[0], fields[1]
    old_ramdisk_size, old_ramdisk_addr = fields[2], fields[3]
    second_size, _second_addr, tags_addr, page_size, dt_size, _unused = fields[4:]
    if (kernel_addr, tags_addr, page_size, dt_size) != (KERNEL_ADDR, TAGS_ADDR, PAGE_SIZE, 0):
        raise SystemExit("ERROR: source Android address/page contract changed")
    if not source[64:576].startswith(b"bootopt=64S3,32N2,32N2"):
        raise SystemExit("ERROR: source bootopt no longer selects the proven 32-bit path")

    if len(zimage) < 0x30 or struct.unpack_from("<I", zimage, 0x24)[0] != ZIMAGE_MAGIC:
        raise SystemExit("ERROR: ARM zImage magic missing")
    if struct.unpack_from("<II", zimage, 0x28) != (0, len(zimage)):
        raise SystemExit("ERROR: zImage start/end fields do not match its file size")

    old_kernel = source[PAGE_SIZE:PAGE_SIZE + old_kernel_size]
    if old_kernel[:4] != MKIMG_MAGIC or old_kernel[8:14] != b"KERNEL":
        raise SystemExit("ERROR: source MediaTek KERNEL header contract changed")
    if old_kernel[14] != 0:
        raise SystemExit("ERROR: source MediaTek KERNEL header name not null-terminated")
    dtb = padded_dtb(raw_dtb)
    payload = zimage + dtb
    mkimg = bytearray(old_kernel[:MKIMG_SIZE])
    struct.pack_into("<I", mkimg, 4, len(payload))
    kernel = bytes(mkimg) + payload

    kernel_file_end = kernel_addr + len(payload)
    if kernel_file_end >= ramdisk_addr:
        raise SystemExit("ERROR: loaded zImage/DTB payload overlaps the ramdisk")
    kernel_runtime_end = None
    system_map_record = None
    if system_map is not None:
        kernel_runtime_end, system_map_record = system_map_end(system_map, kernel_addr)
        if kernel_runtime_end > ATF_START:
            raise SystemExit("ERROR: decompressed kernel reaches the ATF reservation")
        if kernel_runtime_end >= ramdisk_addr:
            raise SystemExit("ERROR: decompressed kernel/BSS overlaps the ramdisk")
    ramdisk_end = ramdisk_addr + len(ramdisk)
    if ramdisk_addr < ATF_END or ramdisk_end > RAMDISK_END_LIMIT:
        raise SystemExit(
            f"ERROR: ramdisk physical range {ramdisk_addr:#x}-{ramdisk_end:#x} is outside "
            f"{ATF_END:#x}-{RAMDISK_END_LIMIT:#x}"
        )

    header = bytearray(source[:PAGE_SIZE])
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<I", header, 16, len(ramdisk))
    struct.pack_into("<I", header, 20, ramdisk_addr)
    old_ramdisk_off = align(PAGE_SIZE + old_kernel_size)
    old_second_off = align(old_ramdisk_off + old_ramdisk_size)
    old_dt_off = align(old_second_off + second_size)
    second = source[old_second_off:old_second_off + second_size]
    outer_dt = source[old_dt_off:old_dt_off + dt_size]
    header[576:608] = android_id(kernel, ramdisk, second, outer_dt)

    result = bytearray(header)
    result += kernel
    result += b"\0" * (align(len(result)) - len(result))
    ramdisk_file_offset = len(result)
    result += ramdisk
    result += b"\0" * (align(len(result)) - len(result))
    result += second
    result += b"\0" * (align(len(result)) - len(result))
    result += outer_dt
    if len(result) > len(source):
        raise SystemExit(f"ERROR: image exceeds the 16 MiB boot envelope by {len(result) - len(source):#x} bytes")
    result += b"\0" * (len(source) - len(result))

    record: dict[str, object] = {
        "android": {
            "image_size": len(result),
            "page_size": PAGE_SIZE,
            "kernel_size": len(kernel),
            "kernel_addr": f"0x{kernel_addr:08x}",
            "ramdisk_size": len(ramdisk),
            "ramdisk_addr": f"0x{ramdisk_addr:08x}",
            "ramdisk_file_offset": f"0x{ramdisk_file_offset:x}",
            "tags_addr": f"0x{tags_addr:08x}",
            "id": bytes(header[576:608]).hex(),
            "source_second_addr_preserved": f"0x{fields[5]:08x}",
        },
        "memory": {
            "loaded_payload": [f"0x{kernel_addr:08x}", f"0x{kernel_file_end:08x}"],
            "decompressed_kernel_end": None if kernel_runtime_end is None else f"0x{kernel_runtime_end:08x}",
            "atf": [f"0x{ATF_START:08x}", f"0x{ATF_END:08x}"],
            "ramdisk": [f"0x{ramdisk_addr:08x}", f"0x{ramdisk_end:08x}"],
            "ramdisk_end_limit": f"0x{RAMDISK_END_LIMIT:08x}",
            "ram_console_start": "0x44400000",
        },
        "mtk": {
            "header_sha256": sha256(bytes(mkimg)),
            "payload_size": len(payload),
            "zimage_size": len(zimage),
            "padded_dtb_size": len(dtb),
            "padded_dtb_sha256": sha256(dtb),
        },
        "system_map": system_map_record,
    }
    return bytes(result), record


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-boot", type=Path, required=True)
    parser.add_argument("--stock-root", type=Path, required=True,
                        help="extracted v184 ARM32 root-adb ramdisk")
    parser.add_argument("--busybox", type=Path, required=True)
    parser.add_argument("--musl-loader", type=Path, required=True)
    parser.add_argument("--audio-probe", type=Path,
                        help="static ARM32 ALSA capability probe to add to the initramfs")
    parser.add_argument("--tinyplay", type=Path,
                        help="static ARM32 TinyALSA playback utility to add to the initramfs")
    parser.add_argument("--tinycap", type=Path,
                        help="static ARM32 TinyALSA capture utility to add to the initramfs")
    parser.add_argument("--tinymix", type=Path,
                        help="static ARM32 TinyALSA mixer utility to add to the initramfs")
    parser.add_argument("--startup-audio", type=Path,
                        help="stereo 48kHz PCM16 WAV to play once after successful init")
    parser.add_argument("--connectivity-stock-root", type=Path,
                        help="pinned v181 ARM32 WMT runtime and firmware root")
    parser.add_argument("--wmt-config-helper", type=Path,
                        help="reviewed static ARM32 configure-only WMT helper")
    parser.add_argument("--wmt-responder", type=Path,
                        help="reviewed static ARM32 Gate2 WMT responder")
    parser.add_argument("--wmt-bt-on", type=Path,
                        help="reviewed static ARM32 one-shot BT-only helper")
    parser.add_argument("--wmt-stock-compat", type=Path,
                        help="proven ARM32 stock-compatible configure-only helper")
    parser.add_argument("--wmt-launcher", type=Path,
                        help="proven ARM32 one-shot WMT command responder")
    parser.add_argument("--wpa-supplicant", type=Path,
                        help="static ARM32 wpa_supplicant 2.10 client")
    parser.add_argument("--wifi-config", type=Path,
                        help="build-local WPA profile; never committed to source")
    parser.add_argument("--qemu-arm", default="qemu-arm-static",
                        help="user-mode ARM emulator used to inventory pinned BusyBox applets")
    parser.add_argument("--zimage", type=Path, required=True)
    parser.add_argument("--expected-zimage-sha256", default=PROVEN_ZIMAGE_SHA256)
    parser.add_argument("--system-map", type=Path, required=True)
    parser.add_argument("--expected-system-map-sha256", default=PROVEN_SYSTEM_MAP_SHA256)
    parser.add_argument("--dtb", type=Path,
                        help="supplied pinned EVT DTB; omit only for the stock-DTB ADB parity stage")
    parser.add_argument("--expected-dtb-sha256")
    parser.add_argument("--ramdisk-address", type=parse_int, default=RAMDISK_ADDR)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ramdisk-output", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    connectivity_options = {
        "connectivity_stock_root": args.connectivity_stock_root,
        "wmt_config_helper": args.wmt_config_helper,
        "wmt_responder": args.wmt_responder,
        "wmt_bt_on": args.wmt_bt_on,
        "wmt_stock_compat": args.wmt_stock_compat,
        "wmt_launcher": args.wmt_launcher,
    }
    connectivity_enabled = all(value is not None for value in connectivity_options.values())
    if any(value is not None for value in connectivity_options.values()) and not connectivity_enabled:
        missing = ", ".join(
            "--" + name.replace("_", "-")
            for name, value in connectivity_options.items() if value is None
        )
        raise SystemExit(f"ERROR: connectivity bundle is all-or-nothing; missing {missing}")
    if connectivity_enabled and not CONNECTIVITY_HELPERS:
        raise SystemExit("ERROR: connectivity helper identities have not been pinned")
    network_options = {"wpa_supplicant": args.wpa_supplicant, "wifi_config": args.wifi_config}
    network_enabled = all(value is not None for value in network_options.values())
    if any(value is not None for value in network_options.values()) and not network_enabled:
        missing = ", ".join(
            "--" + name.replace("_", "-")
            for name, value in network_options.items() if value is None
        )
        raise SystemExit(f"ERROR: network stack is all-or-nothing; missing {missing}")
    audio_tool_options = {
        "tinyplay": args.tinyplay,
        "tinycap": args.tinycap,
        "tinymix": args.tinymix,
    }
    audio_tools_enabled = all(value is not None for value in audio_tool_options.values())
    if any(value is not None for value in audio_tool_options.values()) and not audio_tools_enabled:
        missing = ", ".join(
            "--" + name.replace("_", "-")
            for name, value in audio_tool_options.items() if value is None
        )
        raise SystemExit(f"ERROR: audio tools are all-or-nothing; missing {missing}")
    if audio_tools_enabled and args.audio_probe is None:
        raise SystemExit("ERROR: audio tools require --audio-probe")
    if args.startup_audio is not None and not audio_tools_enabled:
        raise SystemExit("ERROR: startup audio requires --audio-probe and all audio tools")

    source = read(args.source_boot)
    require_hash("source boot envelope", source, SOURCE_BOOT_SHA256)
    zimage = read(args.zimage)
    require_hash("ARM32 zImage", zimage, args.expected_zimage_sha256)
    system_map = read(args.system_map)
    require_hash("ARM32 System.map", system_map, args.expected_system_map_sha256)
    raw_dtb, dtb_origin = extract_or_read_dtb(source, args.dtb, args.expected_dtb_sha256)
    qemu_arm = shutil.which(args.qemu_arm)
    if qemu_arm is None:
        raise SystemExit(f"ERROR: ARM user-mode emulator not found: {args.qemu_arm}")

    output = args.output.resolve()
    ramdisk_output = (args.ramdisk_output or output.with_suffix(".ramdisk.cpio.gz")).resolve()
    manifest_output = (args.manifest or output.with_suffix(".manifest.json")).resolve()
    for path in (output, ramdisk_output, manifest_output):
        if path.exists():
            raise SystemExit(f"ERROR: refusing to overwrite {path}")
        path.parent.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "schema_version": 2,
        "name": "libreecho-mt8163-arm32-v97-recovery",
        "status": "PREPARED_NOT_FLASHED",
        "inputs": {
            "source_boot": {"path": str(args.source_boot.resolve()), "sha256": SOURCE_BOOT_SHA256},
            "zimage": {"path": str(args.zimage.resolve()), "sha256": args.expected_zimage_sha256},
            "system_map": {
                "path": str(args.system_map.resolve()),
                "sha256": args.expected_system_map_sha256,
            },
            "dtb_origin": dtb_origin,
            "dtb_raw_sha256": sha256(raw_dtb),
            "dtb_raw_size": len(raw_dtb),
        },
        "connectivity": {
            "id": CONNECTIVITY_BUNDLE_ID,
            "enabled": False,
            "activation": "manual-gates-only",
            "autostart": False,
            "files": {},
            "helpers": {},
            "symlinks": {},
        },
        "network": {
            "enabled": False,
            "activation": "passive-until-profile-is-supplied",
        },
        "audio": {
            "enabled": False,
            "activation": "manual-only",
            "probe": {},
            "tools": {},
        },
    }
    overlay = Path(__file__).resolve().parent / "initramfs"
    with tempfile.TemporaryDirectory(prefix="libreecho-arm32-initramfs-") as temporary:
        stage = Path(temporary)
        copy_pinned(args.stock_root.resolve(), stage, manifest)
        add_overlay(
            stage, overlay, args.busybox.resolve(), args.musl_loader.resolve(),
            qemu_arm, manifest,
        )
        if args.audio_probe is not None:
            add_audio_probe(stage, args.audio_probe.resolve(), manifest)
        if audio_tools_enabled:
            add_audio_tools(
                stage, args.tinyplay.resolve(), args.tinycap.resolve(),
                args.tinymix.resolve(), manifest,
            )
        if args.startup_audio is not None:
            add_startup_audio(stage, args.startup_audio.resolve(), manifest)
        if connectivity_enabled:
            add_connectivity_bundle(
                args.connectivity_stock_root.resolve(), stage,
                {
                    "wmt_config_helper": args.wmt_config_helper.absolute(),
                    "wmt_responder": args.wmt_responder.absolute(),
                    "wmt_bt_on": args.wmt_bt_on.absolute(),
                    "wmt_stock_compat": args.wmt_stock_compat.absolute(),
                    "wmt_launcher": args.wmt_launcher.absolute(),
                },
                manifest,
            )
        if network_enabled:
            add_network_bundle(
                stage, args.wpa_supplicant.resolve(), args.wifi_config.resolve(), manifest,
            )
        validate_stage(stage)
        cpio = build_cpio(stage, 0)
    ramdisk = gzip.compress(cpio, compresslevel=9, mtime=0)
    if ramdisk[:4] != b"\x1f\x8b\x08\x00" or gzip.decompress(ramdisk) != cpio:
        raise SystemExit("ERROR: deterministic gzip round trip failed")

    boot, package_record = package_boot(
        source, zimage, ramdisk, raw_dtb, args.ramdisk_address,
        args.system_map.resolve(),
    )
    ramdisk_output.write_bytes(ramdisk)
    output.write_bytes(boot)
    manifest["initramfs"] = {
        "cpio_sha256": sha256(cpio),
        "cpio_size": len(cpio),
        "gzip_sha256": sha256(ramdisk),
        "gzip_size": len(ramdisk),
        "path": str(ramdisk_output),
    }
    manifest["package"] = package_record
    manifest["output"] = {"path": str(output), "sha256": sha256(boot), "size": len(boot)}
    manifest_output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    print(f"boot_image={output}")
    print(f"boot_sha256={sha256(boot)}")
    print(f"ramdisk={ramdisk_output}")
    print(f"ramdisk_sha256={sha256(ramdisk)}")
    print(f"manifest={manifest_output}")
    print("status=PREPARED_NOT_FLASHED")


if __name__ == "__main__":
    main()
