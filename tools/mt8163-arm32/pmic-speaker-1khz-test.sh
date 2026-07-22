#!/bin/sh

set -u

echo PMIC_SPEAKER_1KHZ_TEST_START

if [ ! -x /tmp/tinymix ] || [ ! -x /sbin/tinyplay ]; then
	printf '%s\n' 'PMIC_SPEAKER_1KHZ_TEST_ERROR=audio-tool-missing'
	exit 1
fi

restore() {
	/tmp/tinymix -D 0 set 5 Off || true
	/tmp/tinymix -D 0 set 3 Off || true
	/tmp/tinymix -D 0 set 1 Off || true
	/tmp/tinymix -D 0 set 0 Off || true
}
trap restore EXIT

/tmp/tinymix -D 0 set 0 On
r0=$?
/tmp/tinymix -D 0 set 1 On
r1=$?
/tmp/tinymix -D 0 set 3 On
r3=$?
/tmp/tinymix -D 0 set 5 On
r5=$?
printf 'PMIC_SPEAKER_SWITCH_RCS=%s,%s,%s,%s\n' "$r0" "$r1" "$r3" "$r5"
/tmp/tinymix -D 0 get 0
/tmp/tinymix -D 0 get 1
/tmp/tinymix -D 0 get 3
/tmp/tinymix -D 0 get 5

/sbin/tinyplay /tmp/libreecho-tone-1khz.wav -D 0 -d 0 -p 1024 -n 2
play_rc=$?
printf 'PMIC_SPEAKER_1KHZ_TINYPLAY_RC=%s\n' "$play_rc"

if [ "$r0" -ne 0 ] || [ "$r1" -ne 0 ] || [ "$r3" -ne 0 ] ||
	[ "$r5" -ne 0 ] || [ "$play_rc" -ne 0 ]; then
	exit 1
fi

printf '%s\n' 'PMIC_SPEAKER_1KHZ_TEST_RESULT=PASS'
exit 0
