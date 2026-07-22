#!/bin/sh

set -u

echo HPR_ROUTE_1KHZ_TEST_START
if [ ! -x /tmp/tinymix ] || [ ! -x /sbin/tinyplay ]; then
	echo HPR_ROUTE_1KHZ_TEST_ERROR=audio-tool-missing
	exit 1
fi

restore() {
	/tmp/tinymix -D 0 set 64 0 0 || true
	/tmp/tinymix -D 0 set 5 Off || true
	/tmp/tinymix -D 0 set 88 On || true
}
trap restore EXIT

/tmp/tinymix -D 0 set 64 1 1
dac_rc=$?
/tmp/tinymix -D 0 set 88 Off
mfp_rc=$?
/tmp/tinymix -D 0 set 5 On
amp_rc=$?
printf 'HPR_ROUTE_RCS=%s,%s,%s\n' "$dac_rc" "$mfp_rc" "$amp_rc"
/tmp/tinymix -D 0 get 64
/tmp/tinymix -D 0 get 88

/sbin/tinyplay /tmp/libreecho-tone-1khz.wav -D 0 -d 23 -p 1024 -n 2
play_rc=$?
printf 'HPR_ROUTE_1KHZ_TINYPLAY_RC=%s\n' "$play_rc"

if [ "$dac_rc" -ne 0 ] || [ "$mfp_rc" -ne 0 ] || [ "$amp_rc" -ne 0 ] ||
	[ "$play_rc" -ne 0 ]; then
	exit 1
fi

echo HPR_ROUTE_1KHZ_TEST_RESULT=PASS
exit 0
