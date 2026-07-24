#!/bin/busybox sh
set +e
ROOT=/run/libreecho/features/airplay2/root
echo STATUS
ls -l /run/libreecho/airplay.sock 2>&1
echo DBUS
/bin/busybox chroot "$ROOT" /usr/local/sbin/dbus-daemon --nofork --nopidfile \
  --config-file=/etc/dbus-1/system.conf >/tmp/airplay-dbus-test.log 2>&1 &
dbus_pid=$!
/bin/busybox sleep 1
cat /tmp/airplay-dbus-test.log 2>/dev/null
kill "$dbus_pid" 2>/dev/null
wait "$dbus_pid" 2>/dev/null
echo AVAHI
/bin/busybox chroot "$ROOT" /usr/local/sbin/avahi-daemon --no-chroot \
  --no-drop-root --no-rlimits >/tmp/airplay-avahi-test.log 2>&1 &
avahi_pid=$!
/bin/busybox sleep 1
cat /tmp/airplay-avahi-test.log 2>/dev/null
kill "$avahi_pid" 2>/dev/null
wait "$avahi_pid" 2>/dev/null
echo NQPTP
/bin/busybox chroot "$ROOT" /usr/local/sbin/nqptp >/tmp/airplay-nqptp-test.log 2>&1 &
nqptp_pid=$!
/bin/busybox sleep 1
cat /tmp/airplay-nqptp-test.log 2>/dev/null
ls -la "$ROOT/dev/shm" 2>&1
kill "$nqptp_pid" 2>/dev/null
wait "$nqptp_pid" 2>/dev/null
echo SHAIRPORT
/bin/busybox chroot "$ROOT" /usr/local/sbin/shairport-sync \
  --configfile /etc/libreecho/airplay2.conf >/tmp/airplay-shairport-test.log 2>&1 &
shairport_pid=$!
/bin/busybox sleep 2
cat /tmp/airplay-shairport-test.log 2>/dev/null
kill "$shairport_pid" 2>/dev/null
wait "$shairport_pid" 2>/dev/null
rm -f /tmp/airplay-*-test.log
