#!/bin/busybox sh

API=http://127.0.0.1:8080/api/v1
fail=0

printf '%s\n' '=== identity ==='
id
uname -a
printf '%s\n' '=== cpus ==='
grep '^processor' /proc/cpuinfo
printf 'online='; cat /sys/devices/system/cpu/online
printf '%s\n' '=== bluetooth nodes ==='
ls -l /dev/stpbt /dev/stpwmt /sys/class/bluetooth /run/libreecho/bluetooth.sock /var/log/libreecho-btd.log 2>&1
printf '%s\n' '=== services ==='
ps

get() {
    /bin/busybox timeout 12 /bin/busybox wget -qO- "$API$1"
}

change() {
    method=$1
    path=$2
    payload=$3
    /bin/busybox timeout 12 /bin/busybox wget -qO- \
        --header="Content-Type: application/json" \
        --header="X-LibreEcho-CSRF: $csrf" \
        --post-data="$payload" "$API$path"
}

check() {
    label=$1
    response=$2
    rc=$?
    printf '%s rc=%s response=%s\n' "$label" "$rc" "$response"
    if [ "$rc" -ne 0 ] || [ "${response#*\"ok\":true}" = "$response" ]; then
        fail=1
    fi
}

config=$(get /config)
csrf=$(printf '%s' "$config" | sed -n 's/.*"csrf_token":"\([^"]*\)".*/\1/p')
printf 'config csrf_length=%s response=%s\n' "${#csrf}" "$config"
[ "${#csrf}" -eq 64 ] || fail=1

check status "$(get /bluetooth)"
check enable "$(change PUT /bluetooth '{"enabled":true}')"
check status-after-enable "$(get /bluetooth)"
check scan "$(change POST /bluetooth/scan '{}')"
/bin/busybox sleep 5
check scan-status "$(get /bluetooth)"
check scan-stop "$(change POST /bluetooth/scan/stop '{}')"
check final-status "$(get /bluetooth)"

printf 'bluetooth_runtime_test_failures=%s\n' "$fail"
exit "$fail"
