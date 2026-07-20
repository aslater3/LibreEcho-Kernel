#!/usr/bin/env python3
"""Build the pinned MT8163 EVT Wi-Fi candidate DTB.

The only intended changes to the stock EVT tree are the CONSYS ``clocks``
and ``clock-names`` properties.  The complete output hash pins that promise.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import NoReturn


ANDROID_MAGIC = b"ANDROID!"
MKIMG_MAGIC = bytes.fromhex("88168858")
FDT_MAGIC = 0xD00DFEED

SOURCE_BOOT_SIZE = 0x1000000
SOURCE_BOOT_SHA256 = "c0f52a3b079d214495cd3dd22f92fd85695d1b868c58b491a2edb933bc4f6d1a"
PAGE_SIZE = 0x800
MKIMG_SIZE = 0x200
EVT_PAYLOAD_OFFSET = 0x585185
EVT_RAW_SIZE = 0xC875
STOCK_EVT_SHA256 = "f44630ba28f503dd7503bc7cffa2ee96a319acf2f58f1456bb6f5ff23d57dee1"

CONSYS_NODE = "/soc/consys@18070000"
INFRACFG_NODE = "/soc/infracfg@10001000"
STOCK_CONSYS_REG = (
    0x0, 0x18070000, 0x0, 0x200,
    0x0, 0x10007000, 0x0, 0x100,
    0x0, 0x10001000, 0x0, 0x1000,
)
CONSYS_CLOCKS = (0x5, 0x3)
CONSYS_CLOCK_NAMES = "bus"

MAX_FDT_TOTALSIZE = 0x10000
WIFI_EVT_SIZE = 51353
WIFI_EVT_SHA256 = "d5e8b62e14956fb6402c510bfbc784e2e82479daa3183c32cac1e7bc139e9f04"


def fail(message: str) -> NoReturn:
    raise SystemExit(f"ERROR: {message}")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")


def require_hash(label: str, data: bytes, expected: str) -> None:
    actual = sha256(data)
    if actual != expected:
        fail(f"{label} SHA-256 mismatch\nexpected={expected}\nactual={actual}")


def run(command: list[str], label: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit status {result.returncode}"
        fail(f"{label} failed: {detail}")
    return result


def fdt_hex_cells(fdtget: str, dtb: Path, node: str, property_name: str) -> tuple[int, ...]:
    output = run(
        [fdtget, "-t", "x", str(dtb), node, property_name],
        f"reading {node}/{property_name}",
    ).stdout.split()
    try:
        return tuple(int(cell, 16) for cell in output)
    except ValueError:
        fail(f"non-hex cell in {node}/{property_name}: {' '.join(output)}")


def fdt_string(fdtget: str, dtb: Path, node: str, property_name: str) -> str:
    return run(
        [fdtget, "-t", "s", str(dtb), node, property_name],
        f"reading {node}/{property_name}",
    ).stdout.rstrip("\n")


def require_absent(fdtget: str, dtb: Path, node: str, property_name: str) -> None:
    result = subprocess.run(
        [fdtget, str(dtb), node, property_name],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode == 0:
        fail(f"stock {node}/{property_name} is unexpectedly present")
    if "FDT_ERR_NOTFOUND" not in result.stderr:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit status {result.returncode}"
        fail(f"could not prove stock {node}/{property_name} is absent: {detail}")


def fdt_totalsize(data: bytes, label: str) -> int:
    if len(data) < 8:
        fail(f"{label} is shorter than an FDT header")
    magic, total = struct.unpack_from(">II", data)
    if magic != FDT_MAGIC:
        fail(f"{label} FDT magic mismatch: {magic:#010x}")
    if total != len(data):
        fail(f"{label} totalsize {total:#x} differs from file size {len(data):#x}")
    if total > MAX_FDT_TOTALSIZE:
        fail(f"{label} totalsize {total:#x} exceeds the 64 KiB LK envelope")
    return total


def verify_stock(fdtget: str, dtb: Path, data: bytes) -> None:
    require_hash("stock EVT DTB", data, STOCK_EVT_SHA256)
    fdt_totalsize(data, "stock EVT DTB")
    resources = fdt_hex_cells(fdtget, dtb, CONSYS_NODE, "reg")
    if resources != STOCK_CONSYS_REG:
        fail(f"stock CONSYS resource tuple mismatch: {resources!r}")
    if fdt_hex_cells(fdtget, dtb, INFRACFG_NODE, "phandle") != (CONSYS_CLOCKS[0],):
        fail("stock infracfg phandle is not 5")
    require_absent(fdtget, dtb, CONSYS_NODE, "clocks")
    require_absent(fdtget, dtb, CONSYS_NODE, "clock-names")


def verify_wifi(fdtget: str, dtb: Path, data: bytes) -> int:
    total = fdt_totalsize(data, "Wi-Fi EVT DTB")
    if len(data) != WIFI_EVT_SIZE:
        fail(f"Wi-Fi EVT DTB size mismatch: expected={WIFI_EVT_SIZE} actual={len(data)}")
    if fdt_hex_cells(fdtget, dtb, CONSYS_NODE, "reg") != STOCK_CONSYS_REG:
        fail("Wi-Fi EVT DTB changed the stock CONSYS resource tuple")
    if fdt_hex_cells(fdtget, dtb, INFRACFG_NODE, "phandle") != (CONSYS_CLOCKS[0],):
        fail("Wi-Fi EVT DTB changed the infracfg phandle")
    if fdt_hex_cells(fdtget, dtb, CONSYS_NODE, "clocks") != CONSYS_CLOCKS:
        fail("Wi-Fi EVT DTB has the wrong CONSYS clocks cells")
    if fdt_string(fdtget, dtb, CONSYS_NODE, "clock-names") != CONSYS_CLOCK_NAMES:
        fail("Wi-Fi EVT DTB has the wrong CONSYS clock-names value")
    require_hash("Wi-Fi EVT DTB", data, WIFI_EVT_SHA256)
    return total


def extract_stock_evt(source: bytes) -> bytes:
    if len(source) != SOURCE_BOOT_SIZE or source[:8] != ANDROID_MAGIC:
        fail("source is not the pinned 16 MiB Android boot envelope")
    require_hash("source boot envelope", source, SOURCE_BOOT_SHA256)

    kernel_size = struct.unpack_from("<I", source, 8)[0]
    kernel = source[PAGE_SIZE:PAGE_SIZE + kernel_size]
    if kernel[:4] != MKIMG_MAGIC:
        fail("source MediaTek KERNEL header is missing")
    payload_size = struct.unpack_from("<I", kernel, 4)[0]
    payload = kernel[MKIMG_SIZE:MKIMG_SIZE + payload_size]
    end = EVT_PAYLOAD_OFFSET + EVT_RAW_SIZE
    if end > len(payload):
        fail("stock EVT range lies outside the MediaTek kernel payload")
    return payload[EVT_PAYLOAD_OFFSET:end]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Extract the pinned stock EVT DTB and add only the CONSYS bus clock properties."
    )
    parser.add_argument("--source-boot", type=Path, required=True,
                        help="pinned v184 16 MiB stock boot envelope")
    parser.add_argument("--output", type=Path, required=True,
                        help="new raw Wi-Fi candidate DTB (must not already exist)")
    args = parser.parse_args()

    fdtget = shutil.which("fdtget")
    fdtput = shutil.which("fdtput")
    if fdtget is None or fdtput is None:
        missing = ", ".join(
            name for name, path in (("fdtget", fdtget), ("fdtput", fdtput)) if path is None
        )
        fail(f"required device-tree tool not found in PATH: {missing}")
    if args.output.exists():
        fail(f"refusing to overwrite {args.output}")

    source = read(args.source_boot)
    stock = extract_stock_evt(source)
    with tempfile.TemporaryDirectory(prefix="libreecho-wifi-dtb-") as temporary:
        stock_path = Path(temporary) / "stock-evt.dtb"
        candidate_path = Path(temporary) / "wifi-evt.dtb"
        stock_path.write_bytes(stock)
        verify_stock(fdtget, stock_path, stock)

        shutil.copyfile(stock_path, candidate_path)
        run(
            [fdtput, "-t", "x", str(candidate_path), CONSYS_NODE, "clocks", "5", "3"],
            "adding the CONSYS clocks property",
        )
        run(
            [fdtput, "-t", "s", str(candidate_path), CONSYS_NODE, "clock-names", "bus"],
            "adding the CONSYS clock-names property",
        )
        candidate = candidate_path.read_bytes()
        total = verify_wifi(fdtget, candidate_path, candidate)

    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("xb") as output:
            output.write(candidate)
    except OSError as exc:
        fail(f"cannot create {args.output}: {exc}")

    print(
        "wifi_dtb_contract=PASS "
        f"stock_sha256={STOCK_EVT_SHA256} "
        f"output_sha256={WIFI_EVT_SHA256} "
        f"totalsize={total} "
        f"node={CONSYS_NODE} "
        "reg=stock-three-resource clocks=5,3 clock-names=bus"
    )


if __name__ == "__main__":
    main()
