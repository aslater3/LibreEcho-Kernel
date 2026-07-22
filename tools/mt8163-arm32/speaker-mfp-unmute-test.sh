#!/bin/sh

set -u

echo SPEAKER_MFP_UNMUTE_TEST_START
id
uname -a

if [ ! -x /tmp/tinymix ]; then
	printf '%s\n' 'SPEAKER_MFP_UNMUTE_TEST_ERROR=tinymix-missing'
	exit 1
fi

/tmp/tinymix -D 0 set 88 Off
off_rc=$?
printf 'SPEAKER_MFP_MUTE_OFF_RC=%s\n' "$off_rc"
/tmp/tinymix -D 0 get 88
get_off_rc=$?
printf 'SPEAKER_MFP_MUTE_GET_OFF_RC=%s\n' "$get_off_rc"

if [ "$off_rc" -ne 0 ] || [ "$get_off_rc" -ne 0 ]; then
	/tmp/tinymix -D 0 set 88 On || true
	exit 1
fi

sleep 2
cat /proc/uptime

/tmp/tinymix -D 0 set 88 On
on_rc=$?
printf 'SPEAKER_MFP_MUTE_ON_RC=%s\n' "$on_rc"

if [ "$on_rc" -ne 0 ]; then
	exit 1
fi

printf '%s\n' 'SPEAKER_MFP_UNMUTE_TEST_RESULT=PASS'
exit 0
