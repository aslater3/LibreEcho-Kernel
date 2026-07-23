#!/bin/busybox sh

printf '%s\n' '=== btd log ==='
for f in /var/log/libreecho-btd.log /var/log/libreecho.log /var/log/libreecho.log.1 /tmp/libreecho-btd.log; do
    if [ -f "$f" ]; then
        printf '%s\n' "--- $f ---"
        ls -l "$f"
        tail -n 160 "$f"
    fi
done

printf '%s\n' '=== runtime files ==='
ls -la /var/log /run/libreecho /tmp 2>&1

printf '%s\n' '=== bluetooth status ==='
cat /sys/class/bluetooth/hci0/address 2>&1
cat /sys/class/bluetooth/hci0/type 2>&1
cat /sys/class/bluetooth/hci0/name 2>&1
cat /proc/net/bluetooth 2>&1

printf '%s\n' '=== dmesg tail ==='
dmesg | tail -n 180

printf '%s\n' '=== helper inventory ==='
ls -l /sbin/wmt_configure /sbin/wmt_bt_on /sbin/wmt_wifi_on 2>&1
