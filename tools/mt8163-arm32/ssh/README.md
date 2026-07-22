# ARM32 Dropbear input

This directory contains the first SSH implementation slice: a reproducible
server-only Dropbear build contract. It does not yet package Dropbear into the
initramfs or start an SSH daemon.

The selected source is Dropbear 2026.93 from the official release mirror:

```text
https://mirror.dropbear.nl/mirror/releases/dropbear-2026.93.tar.bz2
sha256: 310a6087952897c182efbe16088fa0c4d07c467e850a22699472137278fabf09
signature: 3944eff09b1f9b342e7d6cf214bb256d933142f70cf738c0ba34b1e7465396ed
signing-key fingerprint: F734 7EF2 EE2E 07A2 6762 8CA9 4493 1494 F29C 6773
```

The source archive, detached signature, and signing key are stored in the
canonical pipeline input directory and included in its `SHA256SUMS` closure.
The release signature was verified before the archive was installed there.

Password authentication also uses the ARMHF `libcrypt-dev` and `libcrypt1`
packages from the same Ubuntu release family. Their package hashes are pinned
in the pipeline input manifest; the builder extracts them into its external
work directory and statically links `libcrypt` into Dropbear.

```text
https://ports.ubuntu.com/ubuntu-ports/pool/main/libx/libxcrypt/libcrypt-dev_4.4.36-4build1_armhf.deb
sha256: 58010a8f588477dae4444f3b92636ef4e02aa2d21ab4587d33702299163a5380
https://ports.ubuntu.com/ubuntu-ports/pool/main/libx/libxcrypt/libcrypt1_4.4.36-4build1_armhf.deb
sha256: 198990e999add09e21b0ab3522c94333a0e135656c7f00960f534cec15477a74
```

Build the static ARM32 server and host-key utility with:

```sh
LIBREECHO_PIPELINE_ROOT=/home/andy/workspace/mt8163-arm32-wifi-candidate/pipeline \
  tools/mt8163-arm32/ssh/build_dropbear.sh
```

The output is external to the kernel repository under
`pipeline/work/dropbear-2026.93/`. The compile-time options retain password
authentication and disable server public-key authentication. No root password
or image packaging is performed by this helper yet.

`LIBREECHO_KERNEL_SRC` and `LIBREECHO_KERNEL_OUT` are the corresponding
pipeline overrides for building this worktree without reusing the buttons
worktree or its kernel object directory.
