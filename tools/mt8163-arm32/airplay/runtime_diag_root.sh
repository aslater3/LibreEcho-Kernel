#!/bin/busybox sh
# Read-only diagnostics for the staged AirPlay 2 feature runtime.
set +e
ROOT=/run/libreecho/features/airplay2/root
echo "ROOT=$ROOT"
echo "SHM"
ls -la /dev/shm 2>&1
echo "PROCS"
ps 2>&1 | grep -E 'airplay|dbus|avahi|nqptp|shairport' | grep -v grep
echo "AVAHI_HELP"
/bin/busybox chroot "$ROOT" /usr/local/sbin/avahi-daemon --help 2>&1 | head -80
echo "DBUS_HELP"
/bin/busybox chroot "$ROOT" /usr/local/sbin/dbus-daemon --help 2>&1 | head -80
echo "NQPTP_HELP"
/bin/busybox chroot "$ROOT" /usr/local/sbin/nqptp --help 2>&1 | head -80
echo "SHAIRPORT_HELP"
/bin/busybox chroot "$ROOT" /usr/local/sbin/shairport-sync --help 2>&1 | head -100
echo "CONFIG"
/bin/busybox chroot "$ROOT" /bin/sh -c 'ls -la /etc/hosts /etc/resolv.conf /etc/dbus-1/system.conf /run /var 2>&1; sed -n "1,80p" /etc/dbus-1/system.conf 2>&1'
