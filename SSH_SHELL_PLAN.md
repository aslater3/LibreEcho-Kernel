# LibreEcho MT8163 ARM32 SSH Shell Plan

## Status

- Branch: `agent/arm32-ssh`
- Base: `origin/main` at `c729f687` (buttons merge)
- Worktree: `/home/andy/workspace/mt8163-arm32-ssh`
- Scope: Dropbear build and opt-in image packaging contracts are implemented;
  network-gated daemon startup and hardware validation remain.

The buttons branch and its working files remain in the original worktree:
`/home/andy/workspace/mt8163-arm32-wifi-candidate/LibreEcho-Kernel`.

## Goal and acceptance boundary

Provide a real interactive SSH session to the ARM32 development image, not only
an SSH banner or remote one-shot command. The first acceptance target is:

```text
host -> TCP/22 -> authenticated root password -> root /bin/sh -> PTY on /dev/pts/N
```

The initial development implementation will be intentionally narrow:

- Dropbear server, not OpenSSH, as the first implementation;
- password authentication only;
- root login allowed only with the supplied build-local development password;
- no public-key authentication, `authorized_keys`, or client private key in the image;
- no plaintext password in the image; only the salted root password hash is
  packaged in `/etc/shadow`;
- no TCP forwarding (`-j -k`); and
- no automatic SSH startup unless the image has a working network profile and
  reaches the WLAN/IP-ready gate.

This gives a full BusyBox shell for development while avoiding key material in
the image and avoiding an SSH listener on an image with no network identity.
Password-only root SSH is intentionally limited to the development image and
must use a long, build-local password that is never committed or logged.

## Findings from `origin/main`

### Existing runtime

The image already contains:

- static ARM32 musl BusyBox and `/bin/sh`;
- `setsid`, `getty`, `login`, `passwd`, `su`, `stty`, `id`, `ps`, `ip`, `nc`,
  and `udhcpc` BusyBox applets;
- `/proc`, `/sys`, and `devpts` mounted by `libreecho-init`;
- `/dev/ptmx` created as character device 5:2;
- a working ARM32 `wpa_supplicant`/DHCP path when a build-local profile is
  supplied; and
- the existing PID-1-managed root command runner and independent reboot-to-
  fastboot supervisor.

The live device currently has WLAN address `192.168.0.125`, but no TCP listener
and no SSH/dropbear process. This is runtime evidence from the current device,
not evidence that the new branch has been flashed.

### Kernel closure

A temporary out-of-tree `olddefconfig` resolution from the branch produced:

```text
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IPV6=y
CONFIG_INPUT_EVDEV=y
CONFIG_KEYBOARD_MTK=y
CONFIG_TTY=y
CONFIG_UNIX98_PTYS=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_TMPFS=y
CONFIG_SECURITY=y
CONFIG_SECURITY_NETWORK=y
CONFIG_SECURITY_SELINUX=y
```

No kernel configuration change is expected for the first SSH candidate. If
runtime PTY allocation fails, inspect the exact booted `.config`, `/dev/ptmx`,
`/dev/pts`, and `devpts` mount before changing the kernel.

### Packaging constraints

The initramfs builder currently has a closed, hash-pinned input model:

- `build_recovery_image.py` accepts explicit ARM32 binaries and records them in
  the manifest;
- `verify_recovery_image.py` validates ARM32 ELF identity, hashes, symlinks,
  ownership, init behavior, and image geometry; and
- `pipeline/build.sh` currently hard-codes its kernel source to
  `$ROOT/LibreEcho-Kernel`.

The kernel repository and the `pipeline/` directory are separate Git boundaries.
The SSH implementation must therefore update both the kernel-side packaging
code and the external pipeline/build input closure, or parameterize the
pipeline's kernel source before building the SSH worktree. Never build an SSH
image while the pipeline silently points at the buttons or LED worktree.

## Dropbear versus OpenSSH

### Decision: Dropbear first

Dropbear is the appropriate first target for this image because it is designed
for embedded systems and can provide the required server, password auth, and
PTY shell with a small static ARM32 binary and without the distro service stack.
The host has no Dropbear binary yet, so the target binary must be built and
pinned; the host's amd64 `dropbear`/`openssh-server` packages are not usable as
image inputs.

The Ubuntu package metadata is useful only as a size/dependency signal:

- `dropbear` itself is listed at about 49 KiB installed, excluding its daemon
  package split;
- the distro `openssh-server` package is about 2 MiB and depends on the normal
  glibc/PAM/Kerberos/crypto/system-service ecosystem.

The image should not copy the distro OpenSSH package. An upstream minimal
OpenSSH build could be made static and built without PAM/Kerberos, but it would
still require a larger dependency and configuration audit, more storage, and
more service/account integration than Dropbear.

### OpenSSH fallback criteria

Consider OpenSSH only if a Dropbear candidate fails a required acceptance gate,
for example:

- a required host/client crypto algorithm is unavailable;
- Dropbear's PTY or shell behavior cannot meet the development workflow;
- a future product requirement specifically needs OpenSSH's server features;
  or
- a verified ARM32 static OpenSSH build with an acceptable dependency closure
  becomes available.

If used later, OpenSSH must be built from pinned source with PAM, Kerberos,
GSSAPI, X11, and unnecessary forwarding/features disabled unless explicitly
needed. Its `sshd`, host keys, account files, shell, PTY setup, and every shared
library must be treated as a new closed packaging contract.

## Implementation phases

### Phase 1 — Pin and build Dropbear

Status: implemented in `tools/mt8163-arm32/ssh/`. Dropbear 2026.93 is
signature-verified and hash-pinned, the ARM32 server and `dropbearkey` utility
build statically with password authentication enabled and public-key
authentication compiled out, and repeated builds are byte-for-byte identical.
The target ARMHF `libcrypt` development/runtime packages are explicit,
hash-checked pipeline inputs because the installed cross-toolchain does not
ship them.

1. Select a pinned Dropbear release/commit and record source archive hash and
   provenance in the pipeline input manifest.
2. Add a reproducible ARM32 cross-build helper. Prefer a static target binary;
   do not rely on a dynamic interpreter or libraries absent from the image.
3. Build at least the server and host-key utility. Do not package the client
   unless a later requirement needs outbound SSH.
4. Disable unused footprint/features where supported, while retaining:
   password authentication, PTY allocation, shell execution, modern host-key
   algorithms, and logging needed for bring-up. Disable server public-key
   authentication if the selected Dropbear build supports that compile-time
   option; otherwise omit `authorized_keys` and verify key login is rejected.
5. Verify the output with `readelf`/`file`: little-endian ELF32 ARM, supported
   ARM ABI, no unexpected `PT_INTERP`, no unpinned shared-library dependency,
   deterministic build flags, executable mode, size, and SHA-256.
6. Accept a build-local root password hash rather than a plaintext password
   where possible. Keep the plaintext password outside the repository, image,
   manifest, and build logs.

### Phase 2 — Add explicit SSH packaging contracts

Status: implemented as an explicit `--ssh-enabled` opt-in in the recovery image
builder, verifier, and pipeline. The default candidate remains SSH-disabled;
the enabled path requires a private, non-world-writable salted root hash file,
never records that hash in the manifest/logs, stages no `authorized_keys`, and
does not publish a password-bearing image until that local input is supplied.

The builder and verifier use this all-or-nothing SSH bundle interface:

```text
--dropbear <static ARM32 server>
--dropbearkey <static ARM32 host-key utility>   # if separate utility is used
--ssh-root-password-hash <build-local hash file>
--ssh-enabled                                  # explicit opt-in
```

The staged initramfs should contain, at minimum:

```text
/sbin/dropbear
/sbin/dropbearkey                 # if required by the chosen build
/etc/passwd
/etc/group
/etc/shadow
/etc/dropbear/                    # directory only or generated-key target
```

Use a build-local salted password-hash input rather than accepting or
committing a plaintext password. Do not record the hash in the manifest or
build logs. Reject missing, malformed, symlinked, or world-writable password
inputs. The resulting `/etc/shadow` must be mode `0600` and contain no blank
or locked root password.

Use a minimal account database with root's shell set to `/bin/sh`. Do not add
`authorized_keys`; the first candidate must not offer public-key authentication.

Add verifier checks for:

- exact Dropbear ELF identity/hash/size/mode;
- account-file modes and root shell path;
- salted root password hash format and safe `/etc/shadow` ownership/mode;
- no `authorized_keys` or private-key files in the archive;
- no unexpected dynamic loader/library dependency;
- SSH manifest consistency and all-or-nothing enablement; and
- no SSH startup when the SSH bundle is disabled.

### Phase 3 — Add network-gated daemon startup

Add a dedicated `/sbin/libreecho-ssh` control script rather than embedding all
SSH policy in the main init script. It should provide bounded `start`, `stop`,
and `status` behavior and use explicit paths.

Startup sequence:

1. Require the SSH bundle and `/etc/passwd`/`/etc/shadow` to be present.
2. Require the WLAN interface to exist, have carrier, and have an IPv4 address
   and route from the existing verified network worker.
3. Create a writable runtime directory such as `/tmp/dropbear`.
4. Generate ephemeral host keys on each boot if no persistent storage is yet
   available. Store them in `/tmp/dropbear`; never generate deterministic keys
   from device identity. Record that host-key fingerprints change per boot.
5. Start Dropbear in a supervised foreground mode with explicit port and log
   settings, equivalent to:

```text
 dropbear -F -E -p 0.0.0.0:22 -j -k \
   -r /tmp/dropbear/dropbear_rsa_host_key \
   -r /tmp/dropbear/dropbear_ecdsa_host_key
```

   The exact key types and options must be confirmed against the pinned
   Dropbear build before coding. Do not use root-login/password-disabling
   flags such as `-w` or `-g`; password authentication must remain enabled and
   public-key authentication must remain disabled.
6. Log stable markers such as `ssh-profile-present`, `ssh-hostkeys-ready`,
   `ssh-started:<pid>`, `ssh-listening`, `ssh-exit:<rc>`, and
   `ssh-startup-skipped:<reason>`.
7. If the daemon exits, record the failure and avoid an unbounded restart loop.
   A later service-supervision phase can add controlled restart policy.

Do not start SSH from an Android `.rc` file before the network worker is ready.
The current initramfs intentionally defers optional services until the ADB/
FunctionFS control plane is stable; SSH must preserve that ordering and the
independent reboot supervisor.

### Phase 4 — Host and image verification

Before hardware:

1. Add source-contract tests for the builder/verifier and init script.
2. Build the Dropbear input twice or otherwise verify deterministic output.
3. Run `readelf`, hash, mode, archive-tree, no-private-key, no-authorized-key,
   and password-hash checks.
4. Rebuild the ARM32 kernel image through the pipeline with the SSH worktree
   explicitly selected.
5. Extract the final CPIO and verify the daemon, account files, salted root
   password hash, absence of `authorized_keys`, shell symlinks, `/dev/ptmx`
   setup, host-key generation path, and startup markers.
6. Run the existing unit tests, shell syntax checks, Python compilation,
   `git diff --check`, and the independent image verifier.
7. Confirm the final manifest says `PREPARED_NOT_FLASHED` and binds the SSH
   binary/key/image hashes to one run.

### Phase 5 — Runtime acceptance

Flash only the exact verified image through the normal pipeline after explicit
hardware-test approval. Preserve the opposite slot and UART capture.

Run the gates separately:

1. **Boot/ADB:** candidate kernel identity, ADB `device`, root runner, and no
   panic/watchdog/USB regression.
2. **Network:** WLAN interface, carrier, IPv4 address, route, and gateway reach
   the existing network gate.
3. **Daemon:** process exists; `/proc/net/tcp` or a target-side probe shows TCP/22
   listening; init log contains `ssh-listening`.
4. **Authentication:** the supplied root password succeeds; an incorrect
   password, an empty password, and a public-key attempt are rejected.
5. **Full shell:** from the host run an allocated-PTY session and verify:

```sh
ssh -tt -p 22 root@DEVICE_IP /bin/sh -l
id
printf 'shell=%s\n' "$SHELL"
tty
stty -a
pwd
printf 'pty=%s\n' "$(tty)"
exit
```

   Expected results include UID 0, `/bin/sh`, a `/dev/pts/N` TTY, working
   command execution, and clean exit.
6. **Interactive behavior:** verify Ctrl-C interrupts a foreground command,
   terminal resize reaches the shell, stdin/stdout/stderr remain attached, and
   a second connection is bounded and classified correctly.
7. **Recovery:** while SSH is connected or a bounded SSH command is active,
   verify the independent ADB/root-runner reboot-to-fastboot request still
   works. SSH must never own or block the recovery control path.

Report SSH success only after all lower boot, network, authentication, and PTY
 gates pass. A TCP SYN or SSH banner alone is not a shell result.

## Security and product follow-ups

The first candidate is a development shell, not a production remote-management
policy. Before calling it a final OS service, decide separately:

- whether root SSH is replaced by a non-root `libreecho` account;
- where persistent host keys live and how they are provisioned;
- whether SSH should be disabled by default or enabled only by a physical button
  / provisioning marker;
- whether password-only root SSH is replaced by key provisioning or disabled;
- whether port forwarding, SCP/SFTP, and agent forwarding are needed;
- how logs and failed-auth events are rate-limited and retained; and
- how SSH interacts with SELinux enforcing mode and the eventual native PID-1
  service supervisor.

No plaintext password or private key should enter Git,
`pipeline/inputs/SHA256SUMS`, a manifest, or a boot image. The salted root
password hash is necessarily present in `/etc/shadow` for this development
configuration and must not be copied into logs or manifests.

## Expected implementation files

Kernel repository (`agent/arm32-ssh`):

```text
tools/mt8163-arm32/ssh/                    # build/provenance helpers
 tools/mt8163-arm32/initramfs/libreecho-ssh
 tools/mt8163-arm32/initramfs/libreecho-init
 tools/mt8163-arm32/build_recovery_image.py
 tools/mt8163-arm32/verify_recovery_image.py
 tools/mt8163-arm32/test_recovery_image_tools.py
```

Pipeline boundary (separate from the kernel Git repository):

```text
pipeline/build.sh
pipeline/inputs/SHA256SUMS
pipeline/inputs/<pinned-dropbear-source-or-binary>
pipeline/inputs/<dropbear-build-output>
```

The pipeline must gain an explicit kernel-source override or equivalent
worktree-aware mechanism before the SSH branch is built. Otherwise a successful
build can accidentally package a different checkout, invalidating all results.

## Done definition for the first SSH implementation

The first implementation is complete only when:

- Dropbear source/binary provenance and hashes are pinned;
- the image contains no plaintext password/private key and uses password-only
  root auth;
- the daemon starts only after verified network readiness;
- the builder/verifier/tests enforce the complete SSH bundle;
- a fresh verified image is built from `agent/arm32-ssh`;
- the target accepts the supplied password and rejects an incorrect/empty
  password and public-key authentication;
- `ssh -tt` provides a functioning root BusyBox shell on `/dev/pts/N`; and
- independent ADB/recovery operation remains intact.

Until hardware gates pass, the branch status must remain source/build validated,
not SSH-runtime validated.
