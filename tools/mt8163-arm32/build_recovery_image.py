#!/usr/bin/env python3
"""Build the fail-safe MT8163 ARM32 recovery ramdisk and boot image."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
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
PROVEN_ZIMAGE_SHA256 = "4e144959eb0ffaee91b37d05a0f871863a74f4abb1bad0474c2fec358d5176a6"
PROVEN_SYSTEM_MAP_SHA256 = "527292112edd28e8facf2998eefe2224b08a05b193efc73634cd998e9113ba95"

STOCK_FILES = {
    "init": (0o750, "0564299ebbdd4b76fc00b7f48b434355b484874c2ad013f7c6a3dc5cbd103df7"),
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


def copy_pinned(source_root: Path, stage: Path, manifest: dict[str, object]) -> None:
    copied: dict[str, object] = {}
    for relative, (mode, expected) in STOCK_FILES.items():
        source = source_root / relative
        data = read(source)
        require_hash(f"stock userspace {relative}", data, expected)
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
        copied[relative] = {"sha256": expected, "size": len(data), "mode": f"{mode:04o}"}
    manifest["stock_userspace"] = copied


def add_overlay(stage: Path, overlay: Path, busybox: Path, loader: Path,
                qemu_arm: str, manifest: dict[str, object]) -> None:
    directories = (
        "bin", "dev", "dev/pts", "dev/socket", "dev/usb-ffs", "dev/usb-ffs/adb",
        "lib", "lib/firmware", "proc", "sbin", "sys", "system", "system/bin", "tmp",
    )
    for directory in directories:
        target = stage / directory
        target.mkdir(parents=True, exist_ok=True)
        target.chmod(0o777 if directory == "tmp" else 0o755)

    overlay_files = {
        "default.prop": 0o644,
        "init.rc": 0o644,
        "init.recovery.mt8163.rc": 0o644,
        "libreecho-init": 0o755,
    }
    overlay_manifest: dict[str, object] = {}
    for relative, mode in overlay_files.items():
        data = read(overlay / relative)
        target = stage / relative
        target.write_bytes(data)
        target.chmod(mode)
        overlay_manifest[relative] = {"sha256": sha256(data), "size": len(data), "mode": f"{mode:04o}"}

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

    for path in sorted(stage.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        ident = elf_identity(path)
        if ident is not None and ident != (1, 40):
            raise SystemExit(f"ERROR: non-ARM32 ELF in initramfs: {path} class={ident[0]} machine={ident[1]}")

    for relative in ("init", "sbin/adbd"):
        output = subprocess.run(
            ["readelf", "-l", str(stage / relative)], check=True,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ).stdout
        if "Requesting program interpreter" in output:
            raise SystemExit(f"ERROR: {relative} is not static")

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
    for forbidden in (b"/proc/hps/enabled", b"scaling_governor", b"cpuidle"):
        if forbidden in init_script:
            raise SystemExit(f"ERROR: recovery control script contains forbidden policy override {forbidden!r}")
    properties = read(stage / "default.prop")
    for setting in (b"ro.boot.selinux=permissive", b"ro.secure=0", b"ro.debuggable=1", b"ro.adb.secure=0"):
        if setting not in properties.splitlines():
            raise SystemExit(f"ERROR: recovery property contract lacks {setting!r}")


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
    parser.add_argument("--qemu-arm", default="qemu-arm-static",
                        help="user-mode ARM emulator used to inventory pinned BusyBox applets")
    parser.add_argument("--zimage", type=Path, required=True)
    parser.add_argument("--expected-zimage-sha256", default=PROVEN_ZIMAGE_SHA256)
    parser.add_argument("--system-map", type=Path, required=True)
    parser.add_argument("--expected-system-map-sha256", default=PROVEN_SYSTEM_MAP_SHA256)
    parser.add_argument("--dtb", type=Path,
                        help="source-built EVT DTB; omit only for the stock-DTB ADB parity stage")
    parser.add_argument("--expected-dtb-sha256")
    parser.add_argument("--ramdisk-address", type=parse_int, default=RAMDISK_ADDR)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ramdisk-output", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

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
        "schema_version": 1,
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
    }
    overlay = Path(__file__).resolve().parent / "initramfs"
    with tempfile.TemporaryDirectory(prefix="libreecho-arm32-initramfs-") as temporary:
        stage = Path(temporary)
        copy_pinned(args.stock_root.resolve(), stage, manifest)
        add_overlay(
            stage, overlay, args.busybox.resolve(), args.musl_loader.resolve(),
            qemu_arm, manifest,
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
