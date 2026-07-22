#!/bin/sh

set -u

echo SPEAKER_OUTPUT_LINEOUT_TEST_START
id
uname -a

if [ ! -x /tmp/tinymix ] || [ ! -x /sbin/tinyplay ]; then
	printf '%s\n' 'SPEAKER_OUTPUT_LINEOUT_TEST_ERROR=audio-tool-missing'
	exit 1
fi

/tmp/tinymix -D 0 set 65 1 1
lol_rc=$?
/tmp/tinymix -D 0 set 66 1 1
lor_rc=$?
/tmp/tinymix -D 0 set 88 Off
mfp_rc=$?
/tmp/tinymix -D 0 set 5 On
amp_rc=$?
printf 'SPEAKER_LOL_ROUTE_RC=%s\n' "$lol_rc"
printf 'SPEAKER_LOR_ROUTE_RC=%s\n' "$lor_rc"
printf 'SPEAKER_MFP_UNMUTE_RC=%s\n' "$mfp_rc"
printf 'SPEAKER_EXTAMP_ENABLE_RC=%s\n' "$amp_rc"

if [ "$lol_rc" -ne 0 ] || [ "$lor_rc" -ne 0 ] ||
	[ "$mfp_rc" -ne 0 ] || [ "$amp_rc" -ne 0 ]; then
	/tmp/tinymix -D 0 set 5 Off || true
	/tmp/tinymix -D 0 set 88 On || true
	/tmp/tinymix -D 0 set 65 0 0 || true
	/tmp/tinymix -D 0 set 66 0 0 || true
	exit 1
fi

/tmp/tinymix -D 0 get 65
/tmp/tinymix -D 0 get 66
/sbin/tinyplay /tmp/libreecho-tone-440hz.wav -D 0 -d 23 -p 1024 -n 2
play_rc=$?
printf 'SPEAKER_LINEOUT_TINYPLAY_RC=%s\n' "$play_rc"

/tmp/tinymix -D 0 set 5 Off
amp_off_rc=$?
/tmp/tinymix -D 0 set 88 On
mfp_on_rc=$?
/tmp/tinymix -D 0 set 65 0 0
lol_off_rc=$?
/tmp/tinymix -D 0 set 66 0 0
lor_off_rc=$?
printf 'SPEAKER_EXTAMP_DISABLE_RC=%s\n' "$amp_off_rc"
printf 'SPEAKER_MFP_MUTE_RC=%s\n' "$mfp_on_rc"
printf 'SPEAKER_LOL_ROUTE_OFF_RC=%s\n' "$lol_off_rc"
printf 'SPEAKER_LOR_ROUTE_OFF_RC=%s\n' "$lor_off_rc"

if [ "$play_rc" -ne 0 ] || [ "$amp_off_rc" -ne 0 ] ||
	[ "$mfp_on_rc" -ne 0 ] || [ "$lol_off_rc" -ne 0 ] ||
	[ "$lor_off_rc" -ne 0 ]; then
	exit 1
fi

printf '%s\n' 'SPEAKER_OUTPUT_LINEOUT_TEST_RESULT=PASS'
exit 0
