#!/bin/busybox sh
# Read-only on-device assertions for the AirPlay 2 feature runtime.
set -eu

ROOT=/run/libreecho/features/airplay2/root
PAYLOAD=/data/libreecho/features/airplay2/payload.squashfs
MANIFEST=/data/libreecho/features/airplay2/manifest.json
CONFIG=/data/libreecho/config/web-config.json

mount_line=$(/bin/busybox grep ' /data ' /proc/mounts)
case "$mount_line" in
    *' rw,'*|*' rw '*) ;;
    *) echo 'AIRPLAY_RUNTIME_DATA_NOT_RW'; exit 1 ;;
esac
[ -f "$PAYLOAD" ] || { echo AIRPLAY_RUNTIME_PAYLOAD_MISSING; exit 1; }
[ -f "$MANIFEST" ] || { echo AIRPLAY_RUNTIME_MANIFEST_MISSING; exit 1; }
[ -f "$CONFIG" ] || { echo AIRPLAY_RUNTIME_CONFIG_MISSING; exit 1; }
/bin/busybox grep -q '"integrations"' "$CONFIG" || { echo AIRPLAY_RUNTIME_CONFIG_NOT_PERSISTED; exit 1; }
[ -d "$ROOT" ] || { echo AIRPLAY_RUNTIME_ROOT_MISSING; exit 1; }
for path in \
    "$ROOT/usr/local/sbin/dbus-daemon" \
    "$ROOT/usr/local/sbin/avahi-daemon" \
    "$ROOT/usr/local/sbin/nqptp" \
    "$ROOT/usr/local/sbin/libreecho-airplay-audio" \
    "$ROOT/usr/local/sbin/shairport-sync" \
    "$ROOT/etc/libreecho/airplay2.conf" \
    "$ROOT/etc/dbus-1/system.conf" \
    "$ROOT/dev/shm/nqptp" \
    "$ROOT/run/dbus/system_bus_socket" \
    "$ROOT/run/avahi-daemon/socket"; do
    [ -e "$path" ] || { echo "AIRPLAY_RUNTIME_MISSING:$path"; exit 1; }
done
/bin/busybox grep -q 'output_backend = "pipe"' "$ROOT/etc/libreecho/airplay2.conf" || {
    echo AIRPLAY_RUNTIME_NOT_USING_TINYALSA_PIPE; exit 1;
}
/bin/busybox grep -q 'ignore_volume_control = "yes"' "$ROOT/etc/libreecho/airplay2.conf" || {
    echo AIRPLAY_RUNTIME_DOUBLE_VOLUME_ATTENUATION_ENABLED; exit 1;
}
/bin/busybox grep -q 'output_format = "S16_LE"' "$ROOT/etc/libreecho/airplay2.conf" || {
    echo AIRPLAY_RUNTIME_FORMAT_NOT_S16_LE; exit 1;
}
/bin/busybox grep -q 'output_rate = 48000' "$ROOT/etc/libreecho/airplay2.conf" || {
    echo AIRPLAY_RUNTIME_RATE_NOT_48000; exit 1;
}
/bin/busybox grep -q 'output_channels = 2' "$ROOT/etc/libreecho/airplay2.conf" || {
    echo AIRPLAY_RUNTIME_CHANNELS_NOT_STEREO; exit 1;
}
/bin/busybox grep -q ':1B58 ' /proc/net/tcp || { echo AIRPLAY_RUNTIME_RTSP_PORT_MISSING; exit 1; }
echo AIRPLAY_RUNTIME_CHECK_PASS
