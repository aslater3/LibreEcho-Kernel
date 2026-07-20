# MT8163 ARM32 recovery image

This directory builds the first post-kernel-entry milestone: an ARM32 recovery
image with the v97 fast-iteration behavior.  It is intentionally an ADB and
recovery image, not evidence that Wi-Fi works.

The image combines:

* the reviewed ARM32 `zImage` from `mt8163_arm32_defconfig`;
* the proven Android-v0 and MediaTek `KERNEL` envelope from the stock ARM32
  image;
* the stock EVT DTB for the initial ADB-parity gate only;
* the stock static ARM32 Android `init` and `adbd` binaries;
* ARM32 musl BusyBox, its loader, and 304 relative applet links; and
* the audited v97 recovery flow in `initramfs/libreecho-init`.

The builder pins every borrowed binary by SHA-256.  It also rejects non-ARM32
ELF files, a dynamic `init` or `adbd`, the wrong BusyBox interpreter, an
un-pinned DTB, an invalid zImage range, and overlapping physical ranges.
`qemu-arm-static` is used only at build time to obtain the applet list from the
pinned ARM32 BusyBox binary; the resulting target symlinks are then verified.

## Recovery behavior

Android `init` remains PID 1 so the stock property-backed root ADB behavior is
preserved.  The `libreecho-recovery` service performs these operations:

1. creates direct MMC aliases from sysfs, proves partition 7 is the expected
   20,480-sector `expdb`, writes exactly `FASTBOOT_PLEASE`, and reads it back;
2. creates the stable WMT aliases, leaving dynamically allocated `btif` to
   ueventd/devtmpfs;
3. starts the `/tmp/runme` to `/tmp/result` root command loop;
4. waits two seconds before configuring FunctionFS, starts ARM32 `adbd`, waits
   another three seconds, and enables the gadget; and
5. stays alive even if ADB or an optional driver fails.

The ramdisk contains no HPS, CPU-online, cpufreq, or cpuidle forcing.  Those
workarounds can hide the real SMP and power-management state and are not part
of the recovery contract.

## Build the pinned ADB-parity candidate

The following inputs identify the reviewed candidate:

| Input | SHA-256 |
| --- | --- |
| stock 16 MiB boot envelope | `c0f52a3b079d214495cd3dd22f92fd85695d1b868c58b491a2edb933bc4f6d1a` |
| ARM32 zImage | `4e144959eb0ffaee91b37d05a0f871863a74f4abb1bad0474c2fec358d5176a6` |
| System.map | `527292112edd28e8facf2998eefe2224b08a05b193efc73634cd998e9113ba95` |
| ARM32 BusyBox | `d4c8fd2aea01abd851c703f39b29c0de748b2751e4e1a85cae570fa53ad8f4fb` |
| ARM32 musl loader | `1063871174f1bd4f08f4d330e20b07aeb0820327ee739a4d8d1b644df842cb6b` |

From the kernel repository root:

```sh
python3 tools/mt8163-arm32/build_recovery_image.py \
  --source-boot /home/andy/workspace/echo-evidence/v184-stock32-parity/boot-v184-stock32-parity-stock.img \
  --stock-root /home/andy/workspace/echo-evidence/v184-stock32-parity/rootadb-ramdisk-verify \
  --busybox /home/andy/.local/var/pmbootstrap/chroot_rootfs_amazon-radar/usr/bin/busybox \
  --musl-loader /home/andy/.local/var/pmbootstrap/chroot_rootfs_amazon-radar/usr/lib/ld-musl-armhf.so.1 \
  --zimage /tmp/libreecho-arm32-build.IJwk7v/arch/arm/boot/zImage \
  --system-map /tmp/libreecho-arm32-build.IJwk7v/System.map \
  --output /tmp/libreecho-arm32-recovery-v7.img
```

The reviewed build is reproducible byte-for-byte.  Its outputs are:

| Output | Size | SHA-256 |
| --- | ---: | --- |
| `libreecho-arm32-recovery-v7.img` | 16,777,216 | `1cf0ce6a7a80ad798e9d1675d57eb48a41cf80d705cd37f32d6a0bc3aedd30d4` |
| `libreecho-arm32-recovery-v7.ramdisk.cpio.gz` | 1,933,708 | `0306a285d634411f99d9937d9de52851f7024e11dd38a7164f2996455c98cfb6` |

Verify it independently:

```sh
python3 tools/mt8163-arm32/verify_recovery_image.py \
  --source-boot /home/andy/workspace/echo-evidence/v184-stock32-parity/boot-v184-stock32-parity-stock.img \
  --zimage /tmp/libreecho-arm32-build.IJwk7v/arch/arm/boot/zImage \
  --system-map /tmp/libreecho-arm32-build.IJwk7v/System.map \
  --ramdisk /tmp/libreecho-arm32-recovery-v7.ramdisk.cpio.gz \
  --manifest /tmp/libreecho-arm32-recovery-v7.manifest.json \
  --boot-image /tmp/libreecho-arm32-recovery-v7.img \
  --expected-boot-sha256 1cf0ce6a7a80ad798e9d1675d57eb48a41cf80d705cd37f32d6a0bc3aedd30d4
```

Expected final line:

```text
arm32_recovery_image_contract=PASS android_v0=yes mtk_wrapper=yes zimage=yes evt_dtb=yes initramfs_arm32=yes fastboot_marker=yes root_adb_staged=yes runme=yes memory_disjoint=yes status=PREPARED_NOT_FLASHED
```

## Boot envelope

The verifier checks these disjoint physical ranges:

| Component | Range |
| --- | --- |
| loaded zImage plus padded DTB | `0x40008000-0x406c7b08` |
| decompressed kernel through BSS | ends at `0x40fb6784` |
| ATF reservation | `0x43000000-0x43030000` |
| compressed recovery ramdisk | `0x43478000-0x4365018c` |
| RAM console | starts at `0x44400000` |
| FDT handoff | `0x48000000-0x48010000` |

Loading this larger ramdisk at the stock ARM32 address `0x44000000` would put
it too close to, or into, the reserved RAM-console range.  The v97 address
`0x43478000` is therefore an explicit part of the image contract.

## Device gates

The image is marked `PREPARED_NOT_FLASHED`.  Before advancing to connectivity,
capture all of the following from one boot:

1. UART reaches `fastboot-please-written` (which is emitted only after expdb
   identity and marker read-back pass), `functionfs-mounted`,
   `adbd-started`, `android-usb-enabled`, and `init-ready-pid1-managed`.
2. `adb shell id` reports UID 0 and push/pull both work.
3. A pushed `/tmp/runme` produces `/tmp/result` as root.
4. A controlled reboot returns to fastboot through the expdb marker.
5. The opposite boot slot remains untouched as rollback.

The Amonet shadow-slot wrapper is a separate artifact and is not generated or
flashed by these tools.

## Wi-Fi DTB stage

Omitting `--dtb` deliberately selects the exact stock EVT DTB for the ADB
parity gate.  That DTB has the old three-resource CONSYS layout and no named
`bus` clock, so it is not yet a Wi-Fi candidate for the current driver.

The current source-built `giza_evt.dtb` is 66,135 bytes, which exceeds LK's
proven 64 KiB appended-FDT envelope by 599 bytes.  It must not be forced into
this image.  There is also an unresolved mapping discrepancy that must be
settled before deployment: the active genpd path programs the CONSYS EMI
remap at resource 2 plus `0x320`; the stock tuple starts at `0x10001000`
(giving `0x10001320`), while the current DTS starts at `0x10000000` (giving
`0x10000320`).

A Wi-Fi DTB supplied with `--dtb` must therefore be independently pinned with
`--expected-dtb-sha256`, fit within 64 KiB, provide the named
`CLK_INFRA_PMIC_CONN` `bus` clock, and use a resource layout verified against
the driver path and the working 64-bit implementation.  The firmware READY
gate should be attempted only after those checks and the BT-only gate pass.
