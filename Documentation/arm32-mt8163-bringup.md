# MT8163 ARM32 bring-up status

This document records what has been demonstrated on the target and what is
still required.  In particular, it must not be read as a claim that `wlan0`
works yet.

## Verified kernel entry and `setup_arch()`

The deployed ARM32 image produced this uninterrupted UART marker suffix:

```
HIPVTQUMBWSLDopfmeysbgrucixzAERT
```

The markers before the lower-case section cover decompression, ARM head code,
processor lookup, FDT validation, initial page tables, MMU enable, execution at
the linked virtual address, BSS clearing, entry to `start_kernel()`, and its
first C setup.  The lower-case section brackets the major `setup_arch()`
operations.  Reaching `AERT`, followed by normal printk output, demonstrates
that `setup_arch()` returned and early IRQ/time/console setup continued.

The same boot printed all of the following:

* Linux 3.18.140 running as ARMv7 on physical CPU 0;
* machine model `MT8163` selected from the supplied FDT;
* the expected RAM and reserved-memory layout;
* PSCI v0.1 selected from DT;
* the architected timer and MediaTek GPT initialized; and
* `ttyMT0` and the RAM console enabled.

This moves the known failure boundary beyond decompression, MMU entry, FDT
selection, paging, memblock, and `setup_arch()`.  The initramfs hand-off was
also valid in this test (`0x44000000` through `0x443afe02`, with gzip magic
`1f 8b 08 00`).

## MTEE AArch32 ABI correction

The next reproducible failure was not a generic ARM32 entry problem.  MTEE
logged `tz_client_init: TZ Failed -1`, after which `mtee_probe()` deliberately
called `BUG()`.

Cross-checking the generated AArch32 call sequence with the working firmware
ABI identified the cause.  The old AArch32 path put the system session handle
`0xffff1234` in `r0`; ATF therefore interpreted it as the SMC function ID and
returned `SMC_UNK` (`0xffffffff`).  MT8163 must use the modern MTEE convention:

```
r0 = 0x32000008              /* SMC_MTEE_SERVICE_CALL */
r1 = handle
r2 = operation
r3 = argument/type word
r4 = argument pointer/value
r5 = REE service buffer
smc #0
```

The ARM32 implementation must bind these values to `r0` through `r5`, stop the
REE-service loop on `SMC_UNK`, and propagate that result.  MTEE probe failure is
also made nonfatal: the partially registered character device is cleaned up
and probe returns `-ENODEV` instead of panicking the kernel.  This keeps an
optional secure service from preventing recovery and further driver bring-up.

## Branch baseline

The ARM32 work is based on `main`, not on a separate snapshot of
`golden-v97`.  Commit `9f5caf3d` (`golden-v97`) is an ancestor of current
`main` (`0ec07a22` at the time of this note), so this retains the v97 history
while also retaining the later CONSYS, BTIF, WLAN HIF, firmware-download, and
STP/WMT diagnostic work.  Those later changes are useful evidence and a better
starting point, but they do **not** demonstrate a working `wlan0`.

## Recovery userspace contract

The v97 recovery environment is the behavioral reference for fast iteration.
The ARM32 image should preserve this contract:

1. Provide ARM-EABI BusyBox and a root-capable ARM-EABI `adbd`.
2. Mount the minimal `/proc`, `/sys`, and `/dev` filesystems and create the
   required MMC, WMT, and USB device nodes.  Keep `CONFIG_DEVTMPFS` enabled
   and mount devtmpfs from initramfs so dynamically allocated devices such as
   BTIF are not lost.
3. Write `FASTBOOT_PLEASE` to `expdb` (`/dev/mmcblk0p7` in the v97 layout) as
   early as possible, before optional hardware probes.  A failed boot can then
   return to fastboot for another image.
4. Mount FunctionFS, wait for USB to settle, start `adbd`, then enable the USB
   gadget.  Keep the settle delays that made v97 reliable.
5. Retain the `/tmp/runme` root command loop for rapid experiments.
6. Keep PID 1 alive even when an optional service or device probe fails.

The exact v97 ramdisk cannot be installed verbatim in an ARM32 boot image.  Its
BusyBox and `adbd` are AArch64 ELF binaries (and BusyBox expects
`/sbin/linker64`), so an ARM32 kernel would reject them.  Reuse the audited init
flow, firmware, configuration, and recovery semantics, but replace executable
userspace with ARM-EABI builds.

Reference artifacts are identified by SHA-256:

| Artifact | SHA-256 |
| --- | --- |
| `pmos-boot-v97-ffs-adb.img` | `7d487a5cbdc6ba6acaf1da40233f379fd4cf37830c98530b95001760d33f2ace` |
| `v97-ffs-adb-ramdisk.cpio.gz` | `f4275de87b3b1c685ac7e2b9c992af1516b455c78f646d93352c61919458c54f` |
| `v97-ffs-adb-ramdisk-wifi.cpio.gz` | `d40a0b6d693c876ecd6f807dd7c7e1af58b4095933f3b29e537a85037ced6439` |

These hashes identify reference inputs; they do not make the AArch64 ramdisks
suitable for the ARM32 image.

The larger recovery ramdisk must not be loaded at `0x44000000`: that range
would overlap the DT-reserved RAM console beginning at `0x44400000`.  The v97
ramdisk address `0x43478000` leaves the audited ARM32 recovery archives below
`0x44000000` and above the ATF reservation ending at `0x4302ffff`.  Recheck
this bound from the final compressed size for every packaged image.

## Device-tree requirement for CONSYS/WLAN

Do not pair the later `main` CONSYS code with the old EVT DTB unchanged.  The
old DTB exposes only three CONSYS register regions and has no `bus` clock.  The
later driver maps a fourth SPM register region and requests a clock named
`bus`.  With the old DTB, `of_iomap(..., 3)` and/or `devm_clk_get(..., "bus")`
will fail before meaningful WLAN firmware testing.

The WLAN image therefore needs a separately verified EVT DTB with all four
register regions and the INFRA PMIC-CONN bus clock, or an explicitly tested
driver fallback.  Its boot-image addresses and reserved regions must be
rechecked after any DTB change.

## Milestone order

1. Reproduce v97 recovery behavior with ARM-EABI BusyBox and ADB.
2. Optionally add ADB plus RNDIS/USB networking without weakening recovery.
3. Re-establish stable CONSYS and BTIF initialization with the corrected DTB.
4. Load the matching WLAN firmware, complete WMT/STP/HIF bring-up, and create a
   usable `wlan0` interface.
5. Integrate `LibreEcho-UI` only after the network interface is stable.

Each phase should keep the early `FASTBOOT_PLEASE` path and be preserved as a
known-good artifact before proceeding to the next one.
