#!/usr/bin/env bash
# Build the pinned static ARM32 wireless-tools iwconfig utility.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)"
PIPELINE_ROOT="${LIBREECHO_PIPELINE_ROOT:-$(cd -- "$SCRIPT_DIR/../../../pipeline" && pwd -P)}"
CC="${NETWORK_CC:-/usr/bin/arm-linux-gnueabihf-gcc}"
AR="${NETWORK_AR:-/usr/bin/arm-linux-gnueabihf-ar}"
RANLIB="${NETWORK_RANLIB:-/usr/bin/arm-linux-gnueabihf-ranlib}"

SOURCE_URL="https://archive.ubuntu.com/ubuntu/pool/main/w/wireless-tools/wireless-tools_30~pre9.orig.tar.gz"
SOURCE_SHA256="abd9c5c98abf1fdd11892ac2f8a56737544fe101e1be27c6241a564948f34c63"
WORK_ROOT="$PIPELINE_ROOT/work/wireless-tools-30"
ARCHIVE="$WORK_ROOT/wireless-tools_30~pre9.orig.tar.gz"
SOURCE_DIR="$WORK_ROOT/wireless_tools.30"
OUTPUT_DIR="$WORK_ROOT/output"

[[ -x "$CC" ]] || { echo "ERROR: ARM32 network compiler not found: $CC" >&2; exit 1; }
[[ -x "$AR" ]] || { echo "ERROR: ARM32 network archiver not found: $AR" >&2; exit 1; }
[[ -x "$RANLIB" ]] || { echo "ERROR: ARM32 network ranlib not found: $RANLIB" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "ERROR: curl is required for the pinned network source" >&2; exit 1; }
command -v make >/dev/null 2>&1 || { echo "ERROR: make is required for the pinned network source" >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "ERROR: tar is required for the pinned network source" >&2; exit 1; }

mkdir -p "$WORK_ROOT"
if [[ ! -f "$ARCHIVE" ]]; then
  temporary_archive="$ARCHIVE.tmp.$$"
  curl --fail --location --retry 3 --connect-timeout 15 --max-time 120 \
    "$SOURCE_URL" -o "$temporary_archive"
  mv -f -- "$temporary_archive" "$ARCHIVE"
fi
printf '%s  %s\n' "$SOURCE_SHA256" "$ARCHIVE" | sha256sum -c -

rm -rf -- "$SOURCE_DIR" "$OUTPUT_DIR"
tar --no-same-owner --no-same-permissions -xzf "$ARCHIVE" -C "$WORK_ROOT"
[[ -f "$SOURCE_DIR/Makefile" && -f "$SOURCE_DIR/iwconfig.c" ]] || {
  echo "ERROR: pinned wireless-tools source layout changed" >&2
  exit 1
}

make -C "$SOURCE_DIR" \
  CC="$CC" AR="$AR" RANLIB="$RANLIB" \
  LDFLAGS="-static -Wl,--build-id=none" iwconfig

mkdir -p "$OUTPUT_DIR"
cp -- "$SOURCE_DIR/iwconfig" "$OUTPUT_DIR/iwconfig"
chmod 0755 "$OUTPUT_DIR/iwconfig"

readelf_output="$(readelf -h -l -d "$OUTPUT_DIR/iwconfig")"
grep -Eq 'Class:[[:space:]]+ELF32' <<< "$readelf_output"
grep -Eq 'Machine:[[:space:]]+ARM' <<< "$readelf_output"
grep -Eq 'Flags:.*0x(05000400|5000400)' <<< "$readelf_output"
! grep -q 'Requesting program interpreter' <<< "$readelf_output"
! grep -q 'NEEDED' <<< "$readelf_output"
! grep -q 'There is a dynamic section' <<< "$readelf_output"

iwconfig_sha256="$(sha256sum "$OUTPUT_DIR/iwconfig" | awk '{print $1}')"
echo "wireless_tools_source_sha256=$SOURCE_SHA256"
echo "iwconfig_sha256=$iwconfig_sha256"
