#!/bin/sh

set -u

echo SPEAKER_OUTPUT_TEST_START
id
uname -a

if [ ! -x /tmp/tinymix ] || [ ! -x /sbin/tinyplay ]; then
	printf '%s\n' 'SPEAKER_OUTPUT_TEST_ERROR=audio-tool-missing'
	exit 1
fi

/tmp/tinymix -D 0 set 5 On
set_rc=$?
printf 'SPEAKER_EXTAMP_SET_RC=%s\n' "$set_rc"
/tmp/tinymix -D 0 get 5
get_rc=$?
printf 'SPEAKER_EXTAMP_GET_RC=%s\n' "$get_rc"

if [ "$set_rc" -ne 0 ] || [ "$get_rc" -ne 0 ]; then
	/tmp/tinymix -D 0 set 5 Off || true
	exit 1
fi

/sbin/tinyplay /tmp/libreecho-tone-440hz.wav -D 0 -d 23 -p 1024 -n 2
play_rc=$?
printf 'SPEAKER_TINYPLAY_RC=%s\n' "$play_rc"

/tmp/tinymix -D 0 set 5 Off
off_rc=$?
printf 'SPEAKER_EXTAMP_OFF_RC=%s\n' "$off_rc"

if [ "$play_rc" -ne 0 ] || [ "$off_rc" -ne 0 ]; then
	exit 1
fi

printf '%s\n' 'SPEAKER_OUTPUT_TEST_RESULT=PASS'
exit 0
