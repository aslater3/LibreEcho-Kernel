# MT8163 ARM32 connectivity gate helpers

These three small static ARM32 programs keep WMT configuration, command
response, and activation in separate, reviewable executables.  None runs
automatically and the build/test flow never opens a live WMT device.

## Programs

`wmt_responder` implements only the conn_soc userspace command protocol:
`poll()` -> `read()` -> log -> `write()`.  Its default response is the failure
token `fail`; the success token is possible only with an explicit `--ok`.
`--once` provides the bounded responder used by the configuration gate.  It
sets only the launcher-present state (`0x4004a00d`), which does not activate a
connectivity function.

`wmt_configure` is configure-only by construction.  It has no activation
request or function selector.  Before opening `/dev/stpwmt`, it validates both
stock patch files, registers their fixed 264-byte ARM32 records, and selects
full BTIF transport.  `--inspect-patches` performs the same validation offline
and never opens the WMT device.

The pinned patch contract, recovered from the exact stock ARM32
`wmt_launcher` machine code, is:

| File | Size | Bytes 22-23 | Raw route 24-27 | Seq | Registered address | SHA-256 |
| --- | ---: | --- | --- | ---: | --- | --- |
| `ROMv2_lm_patch_1_0_hdr.bin` | 128,720 | `8a:00` | `22:00:06:00` | 2 | `00:00:06:00` | `b4460117f51a43f3284594ec08d8c8861ecc0e42b17820987da03ecabdebac1e` |
| `ROMv2_lm_patch_1_1_hdr.bin` | 50,148 | `8a:00` | `21:00:0e:f0` | 1 | `00:00:0e:f0` | `10c4ed22a10b8a136bffd7ffce4d552300d76f8e593627d2a9841c3b11a5697e` |

The first route byte is not an address byte.  Stock decodes its high nibble as
the total patch count and its low nibble as `downloadSeq`, then clears that
byte before submitting the four-byte address in ioctl 15.  Reading from byte
23 or assigning sequence numbers by filename order shifts both destinations
and swaps both patches.  The utility validates the filename, regular-file
size, exact header and route bytes, decoded count/sequence, and final address.
Gate staging must independently verify the listed whole-file hashes before
execution.

`wmt_bt_on` contains exactly one activation call: ARM32 request `0x4004a006`,
argument `0x80000000` (BT type 0 with the on bit set), once, without retry.  It
has no generic function selector or Wi-Fi option and refuses to run unless
`--execute-bt-only-once` is present.

This BT-only diagnostic is intentionally distinct from stock launcher's WMT
bootstrap.  On MT8163 the stock launcher uses ioctl 7 (`LPBK_POWER_CTRL=1`)
after selecting BTIF, and retries that bootstrap before servicing
`srh_patch`; it does not use ioctl 6 to turn Bluetooth on at that point.  Gate
4 executes the exact stock tools to retain that distinction.

## Reproducible `/tmp` build and offline checks

From the repository root:

```sh
make -C tools/mt8163-arm32/connectivity \
  OUT_DIR=/tmp/libreecho-mt8163-connectivity all check
```

The Makefile uses `arm-linux-gnueabihf-gcc`, links statically, rejects an ELF
interpreter, runs argument and patch-inspection checks under
`qemu-arm-static`, and traces the BT helper against `/dev/null` to prove one
ioctl with the pinned request and argument.  Outputs and test logs are written
only beneath `OUT_DIR`.

The pinned outputs from GCC 13.3.0, binutils 2.42, and QEMU ARM 8.2.2 are:

| Helper | Size | SHA-256 |
| --- | ---: | --- |
| `wmt_configure` | 428,704 | `2fa1c78546b3a0d35442ffa196f3eaa13b1ce4609b537332b016bc88ea663be2` |
| `wmt_responder` | 428,796 | `e20bdaf559165077ff8211c64ed38a10ecee1006641e94302cf14d3be397c350` |
| `wmt_bt_on` | 424,540 | `4365c1b1046bf2ce1045a3fbd4578ee21d8f1a9900a01cb0cde9cea478821d82` |

The recovery-image builder's pinned size, hash, and ELF checks are the final
acceptance gate if the host toolchain changes.

## Gate sequence

Each gate is manual.  There is no init service and no automatic BT or WLAN
activation.  A failed hardware action ends that boot: collect UART and
`dmesg`, retain the expdb fastboot marker, and reboot instead of retrying.

### Gate 0: passive fresh boot

Do not open WMT, run either stock launcher, execute a helper, or write to
`/dev/wmtWifi`.  Confirm the v97 recovery contract first: root ADB, the
`/tmp/runme` loop, verified `FASTBOOT_PLEASE`, expected WMT/BTIF nodes, and
baseline UART/`dmesg` captured before any optional connectivity action.

### Gate 1: offline patch inspection

Inspect the staged patches without opening WMT:

```sh
/sbin/wmt_configure \
  --inspect-patches --firmware-dir /lib/firmware
```

Require two validated `patch_info` lines and `device_opened=no`.

### Gate 2: configure only

Arm one explicit, one-shot success responder and run the configurator.  This
stage contains no function-on operation:

```sh
/sbin/wmt_responder \
  --device /dev/stpwmt --ok --once &
responder_pid=$!
/sbin/wmt_configure \
  --device /dev/stpwmt --firmware-dir /lib/firmware
```

If configuration emits no `srh_patch` command, the one-shot responder remains
armed.  Prove that the recorded PID is still the only responder before moving
to the BT-only gate.

### Gate 3: one BT-only activation

Only after Gate 2 and its stability window, issue exactly one BT-only action
in this boot:

```sh
/sbin/wmt_bt_on \
  --device /dev/stpwmt --execute-bt-only-once
wait "$responder_pid"
```

Do not retry it.  There is intentionally no Wi-Fi activation path in any of
the three helpers.

### Gate 4: exact stock-runtime A/B test

Use another fresh boot; do not mix this comparison with Gates 2 or 3.  Run the
exact pinned v181 Bionic tools:

```sh
/system/vendor/bin/wmt_loader
/system/vendor/bin/wmt_launcher -p /vendor/firmware/ \
  >/tmp/wmt-launcher.log 2>&1 &
launcher_pid=$!
```

The recovered `init.connectivity.rc` is hash-checked as reference evidence but
is not installed.  Missing optional/debug dispatcher calls in the stock
launcher are known compatibility observations, not proof of successful
activation.  This exact launcher performs the ioctl-7 bootstrap and the
stock byte-24 patch-route decoding described above.  Require one live launcher
with the recorded PID and captured patch/init success before declaring the
gate stable.

### Gate 5: one Wi-Fi function-on

Only after Gate 4 is stable, use another fresh boot.  Repeat the exact Gate 4
loader/launcher bootstrap once on that boot, keeping the single launcher
running as the WMT command responder.  After the same patch/init success
evidence and stability window, perform exactly one write:

```sh
/system/vendor/bin/wmt_loader
/system/vendor/bin/wmt_launcher -p /vendor/firmware/ \
  >/tmp/wmt-launcher.log 2>&1 &
launcher_pid=$!
# Advance only after the one launcher reaches the verified bootstrap/patch gate.
printf '1' > /dev/wmtWifi
```

Poll `/sys/class/net/wlan0` for at most 30 seconds.  Do not issue a second
write, start another launcher, or retry in the same boot.  On failure,
preserve UART, `dmesg`, `/tmp/wmt-launcher.log`, and the recorded launcher PID,
then reboot.  A persistent, usable `wlan0` is the milestone; staging firmware
or reaching an intermediate WMT callback is not.
