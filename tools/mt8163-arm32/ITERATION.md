# LibreEcho ARM32 Development OS — Fast Iteration Runbook

## What this image is

This is the **LibreEcho ARM32 Development OS Base** for the Amazon Echo 2nd Gen
(BISCUIT / radar_puffin / MT8163). It is the beginning of the final LibreEcho
OS, not a disposable recovery image.

It currently combines a production-directed ARM32 kernel with a deliberately
small bring-up userspace. Android `init` and stock static ARM32 `adbd` remain in
place temporarily because they provide a proven property area, SELinux setup,
FunctionFS ADB transport, and a controlled root command path while the native
LibreEcho service layer is built.

Some internal filenames still contain `recovery` (`build_recovery_image.py`,
`verify_recovery_image.py`, and `libreecho-recovery`). Those names are retained
for artifact compatibility. They describe the ancestry and safety role of the
current initramfs, not the intended product. Rename them only as a separately
reviewed migration that updates every script, hash pin, manifest, and test.

## Verified development baseline

| Item | Verified value |
| --- | --- |
| Source tag | `arm32-dev-base-2026-07-21` |
| Source commit | `020be03fb4cdd42764b1a0c3211538781eecc575` |
| Current docs commit | `409df69046003aabae357d84f40dc0718f369554` |
| Branch | `agent/arm32-v97-wlan` |
| Device | BISCUIT, serial `G2A0RF0485020316` |
| Last hardware-verified boot image | `afe2d0d932f29f4028523e26d319361c4e9777308f6bcfaf133c57a493865054` |
| Fresh clean rebuild, not flashed | `669aad1edc1a77bd9d76e2b6609d659142ba7ee7aa2cfaec75b5899a12a43ae5` |
| Runtime | ARM32 `armv7l`, root `/tmp/runme`, ADB state `device` |

The kernel must retain:

```text
CONFIG_AEABI=y
CONFIG_OABI_COMPAT=y
CONFIG_USB_MTK_HDRC=y
CONFIG_USB_G_ANDROID=y
CONFIG_USB_FUNCTIONFS unset
```

Stock bionic adbd calls ARM EABI `__ARM_NR_set_tls` (`0x000f0005`). Removing
`CONFIG_AEABI` makes the kernel reject that valid call and kill adbd with
SIGILL/exit 132. The canonical build script fails closed if EABI or OABI
compatibility is absent.

## Workspace and tools

The complete reusable agent procedure is:

```text
~/.hermes/skills/devops/libreecho-mt8163-iteration/SKILL.md
```

Use that skill together with this project runbook; it is the maintained checklist
for live-candidate discovery, build/flash gates, bounded WMT tests, and autonomous
reboot recovery.

```text
Project root: /home/andy/workspace/mt8163-arm32-wifi-candidate
Kernel:       LibreEcho-Kernel/
Pipeline:     pipeline/
UART:         /dev/ttyUSB0 at 921600 8N1
ADB serial:   G2A0RF0485020316
```

Only the pipeline may produce or flash an active image. Do not flash a raw
`zImage`, an arbitrary `boot.img`, or an image copied from an old run.

After a cleanup, `pipeline/work` and `pipeline/out` are intentionally absent.
The first `./build.sh` recreates them from the committed source and pinned
`pipeline/inputs`; the first clean rebuild after the 2026-07-21 cleanup passed
with boot SHA-256 `669aad1edc1a77bd9d76e2b6609d659142ba7ee7aa2cfaec75b5899a12a43ae5`
and was deliberately removed again to leave a clean starting point.

## Before every iteration

1. Confirm only one UART reader owns `/dev/ttyUSB0`. Two readers corrupt or
   split the evidence.
2. Start a new run-specific UART capture before rebooting.
3. Confirm the source branch and dirty state.
4. Decide whether the change is userspace-only or requires a kernel image.
5. Keep the opposite Android boot slot untouched as rollback.

Host UART setup:

```sh
stty -F /dev/ttyUSB0 921600 cs8 -cstopb -parenb \
  -ixon -ixoff -crtscts -echo raw
```

Do not set the CH340 console to 115200. The BROM `/dev/ttyACM*` transport uses
115200; the Linux/LK console on `/dev/ttyUSB0` uses 921600.

## Fast path A: userspace or driver experiment without flashing

Use this whenever the kernel and packaged initramfs do not need to change.
It avoids a reboot and is the safest, fastest loop.

Check ADB transport:

```sh
adb -s G2A0RF0485020316 get-state
adb -s G2A0RF0485020316 get-serialno
```

Expected state is `device`.

### Root access during development

Interactive `adb shell` is not the acceptance or root-control path. It may be
closed, non-root, return empty output, or behave differently from ADB sync.
Do not interpret an empty `adb shell` response as loss of ADB.

Use the deliberate root script runner built into the development OS:

```sh
cat >/tmp/probe.sh <<'EOF'
#!/bin/sh
id
uname -a
cat /proc/cmdline
EOF

cd /home/andy/workspace/mt8163-arm32-wifi-candidate/LibreEcho-Kernel
ADB_SERIAL=G2A0RF0485020316 \
  tools/mt8163-arm32/adb-run-root.sh /tmp/probe.sh
```

The helper:

1. confirms the requested serial is in ADB state `device`;
2. adds a unique nonce to prevent accepting a stale `/tmp/result`;
3. pushes the script to `/tmp/runme`;
4. waits for the PID-1-managed root runner;
5. prints `/tmp/result`; and
6. returns the remote script's exit status, including scripts that call `exit`
   directly (the helper uses a remote `EXIT` trap).

The `/tmp/runme` runner is an intentional development facility, not an
exploit. Use it for module loading, device-node inspection, bounded WMT tests,
log capture, and controlled reboots.

### Safe userspace experiment pattern

A hardware-control script should:

- print a unique start marker;
- record `id`, kernel identity, and relevant device-node identities;
- use bounded timeouts around every ioctl/write that may block;
- trigger one subsystem transition only;
- print a result marker and exit status;
- avoid changing boot partitions or boot-control data; and
- leave cleanup steps in the same script where practical.

Never repeatedly trigger WMT/Wi-Fi merely because ADB disappeared. Loss of ADB
may indicate a kernel panic, watchdog reset, blocked hardware transaction, or
USB failure. Read UART and classify the failure first.

## Fast path B: kernel or packaged-OS iteration

From the pipeline directory:

```sh
cd /home/andy/workspace/mt8163-arm32-wifi-candidate/pipeline

./build.sh                 # incremental kernel build, package, verify
./status.sh                # read-only verification of out/CURRENT
./flash.sh --preflight     # verify image and exact BISCUIT fastboot target
./flash.sh                 # re-arm BCB, flash selected slot, clear expdb, reboot
```

Use this after changing kernel source, defconfig, initramfs content, builders,
or verifier contracts. `./iterate.sh` combines build and flash, but use the
four explicit steps when investigating a risky kernel/hardware change because
they provide useful pause points.

Use `./build.sh --defconfig` only when intentionally regenerating from
`mt8163_arm32_defconfig`. The incremental build uses `pipeline/work/kernel`.
Every successful build is immutable under `pipeline/out/runs/`; only a fully
verified run becomes `pipeline/out/CURRENT`.

### What `flash.sh` changes

The normal slot-A deployment performs these writes in this order:

1. regenerate and flash a BCB image that gives both slots seven tries;
2. flash the verified image to `boot_a`;
3. clear `expdb` immediately before reboot; and
4. issue fastboot reboot.

It does not flash the Amonet wrapper unless explicitly given `--wrapper`.
Use `--wrapper` only when the wrapper changed and has been separately reviewed.

If any verification or fastboot write fails, the script stops and leaves the
device in fastboot. Do not manually reboot a partially written sequence.

## Entering fastboot from the running development OS

Use the validated helper:

```sh
cd /home/andy/workspace/mt8163-arm32-wifi-candidate/LibreEcho-Kernel
ADB_SERIAL=G2A0RF0485020316 \
  tools/mt8163-arm32/adb-reboot-fastboot.sh
```

The target-side request verifies:

- `/dev/mmcblk0p7` is a block device;
- sysfs identifies it as `PARTNAME=expdb`;
- its size is exactly 20,480 sectors;
- `FASTBOOT_PLEASE` reads back after writing; and
- the data is synced before force reboot.

The host then polls the exact serial until fastboot appears.

### Critical expdb behavior

`FASTBOOT_PLEASE` in `expdb` intentionally requests fastboot. If the marker is
left in place, subsequent boots returning to fastboot are expected behavior,
not proof of a new kernel failure.

When deploying a normal boot from fastboot, use `pipeline/flash.sh`; it clears
`expdb` immediately before reboot. Do not casually run `fastboot erase expdb`
outside the documented sequence, because the early marker is part of panic
recovery. If you intentionally clear it by hand, understand that you are
removing the next-reset fastboot escape until the development OS writes it
again.

## Kernel panic and watchdog risk

Kernel and connectivity development can cause:

- synchronous kernel panic/Oops;
- hardware watchdog reset;
- blocked task with no immediate reset;
- USB gadget disconnect while the kernel remains alive;
- ADB loss while UART continues;
- reset before userspace can save `/tmp/result`; or
- failure before the marker is written.

The development OS mitigates—but cannot eliminate—these risks:

1. a built-in kernel marker thread writes `FASTBOOT_PLEASE` early;
2. the userspace control service independently verifies and writes it;
3. BCB is re-armed before each pipeline deployment;
4. the opposite boot slot remains untouched; and
5. UART remains the primary evidence channel.

### Stop conditions

Stop the iteration and inspect evidence when any of these occurs:

- `Kernel panic`, `Oops`, `BUG`, `data abort`, or `undefined instruction`;
- LK/ATF reports `wdt_status`, hardware WDT, or boot retry failure;
- ADB disappears during a hardware write or ioctl;
- a task remains blocked beyond the test's intended timeout;
- fastboot product/serial differs from BISCUIT / `G2A0RF0485020316`;
- any pipeline verifier or hash check fails; or
- the UART capture is missing or has competing readers.

Do not flash a second speculative fix before preserving the first run's UART,
`out/CURRENT`, run directory, image hash, source diff, and exact last marker.

## Recovery decision tree

### ADB is still present

1. Do not reboot blindly.
2. Capture state through `adb-run-root.sh`.
3. Save kernel log, uptime, blocked tasks, interfaces, and device nodes.
4. Use `adb-reboot-fastboot.sh` when a new kernel must be deployed.

### Fastboot is present

1. Confirm exact identity:

   ```sh
   fastboot devices -l
   fastboot -s G2A0RF0485020316 getvar product
   ```

2. Run `pipeline/flash.sh --preflight`.
3. Flash only through `pipeline/flash.sh`.
4. If a write fails, leave the device in fastboot and diagnose the host/cable
   or artifact; do not reboot.

### Neither ADB nor fastboot is present, but UART is active

- Determine whether the kernel is alive, blocked, panicking, or rebooting.
- If it is alive but USB is broken, wait for or induce only the established
  watchdog/reset path if the experiment defined one.
- If it resets after the early marker was written, the next boot should request
  fastboot.
- Do not infer a brick from temporary USB absence.

### No ADB, no fastboot, and no useful UART progress

This is outside the normal loop. Use the archived, established BROM recovery
procedure only after confirming the device is not merely waiting through BCB
retries. BROM/CLK-to-GND recovery is a last resort and is not part of routine
kernel iteration. Keep BROM and UART transports distinct and do not run two
flash/control tools against the device simultaneously.

## Post-boot acceptance gate

A kernel/OS iteration is not accepted merely because Linux prints a banner.
Require one boot to show all of the following:

```text
fastboot-please-written
functionfs-mounted
adbd-started:<pid>
FunctionFS ready opened=1
android-usb-enabled
functionfs-ready
USB_STATE=CONFIGURED
```

Then verify from the host:

```sh
adb -s G2A0RF0485020316 get-state
```

And verify root control:

```sh
cat >/tmp/accept.sh <<'EOF'
id
uname -a
[ -c /dev/null ] && echo dev_null_ok
[ -c /dev/zero ] && echo dev_zero_ok
[ -e /dev/usb-ffs/adb/ep1 ] && echo ffs_ep1_ok
[ -e /dev/usb-ffs/adb/ep2 ] && echo ffs_ep2_ok
EOF

tools/mt8163-arm32/adb-run-root.sh /tmp/accept.sh
```

Expected output includes:

```text
uid=0 gid=0
armv7l
dev_null_ok
dev_zero_ok
ffs_ep1_ok
ffs_ep2_ok
```

Only after this gate should a connectivity or service-layer experiment begin.

## Moving toward the final LibreEcho OS

The current development base is intentionally conservative. Evolve it toward
the final OS in independent, reversible stages:

1. keep the verified ARM32 kernel, boot envelope, DTB constraints, ADB, marker,
   and root runner unchanged;
2. add native LibreEcho service supervision and persistent logs;
3. bring up storage/data partitions without replacing the control plane;
4. bring up networking and Wi-Fi through bounded, manually activated gates;
5. add audio, LED, update, and application services one subsystem at a time;
6. replace Android `init`/adbd only after an equivalent native control plane
   proves root administration, USB transport, panic recovery, and rollback;
7. rename internal `recovery` artifacts only after all consumers and manifests
   migrate atomically; and
8. remove development-only diagnostics and `/tmp/runme` only when the final OS
   has a secure, documented administrative interface and recovery strategy.

“Final OS” does not mean removing recovery properties. The shipping system
still needs a reliable bootloader escape, rollback image, logs, and a bounded
update path; those mechanisms should become product recovery features rather
than disappear.
