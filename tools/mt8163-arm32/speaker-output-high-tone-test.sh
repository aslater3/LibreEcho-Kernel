#!/bin/sh

set -u

echo SPEAKER_OUTPUT_HIGH_TONE_TEST_START
id
uname -a

if [ ! -x /tmp/tinymix ] || [ ! -x /sbin/tinyplay ]; then
	printf '%s\n' 'SPEAKER_OUTPUT_HIGH_TONE_TEST_ERROR=audio-tool-missing'
	exit 1
fi

/tmp/tinymix -D 0 set 88 Off
mfp_rc=$?
/tmp/tinymix -D 0 set 5 On
amp_rc=$?
printf 'SPEAKER_MFP_UNMUTE_RC=%s\n' "$mfp_rc"
printf 'SPEAKER_EXTAMP_ENABLE_RC=%s\n' "$amp_rc"

if [ "$mfp_rc" -ne 0 ] || [ "$amp_rc" -ne 0 ]; then
	/tmp/tinymix -D 0 set 5 Off || true
	/tmp/tinymix -D 0 set 88 On || true
	exit 1
fi

/sbin/tinyplay /tmp/libreecho-tone-4khz.wav -D 0 -d 23 -p 1024 -n 2
play_rc=$?
printf 'SPEAKER_HIGH_TONE_TINYPLAY_RC=%s\n' "$play_rc"

/tmp/tinymix -D 0 set 5 Off
amp_off_rc=$?
/tmp/tinymix -D 0 set 88 On
mfp_on_rc=$?
printf 'SPEAKER_EXTAMP_DISABLE_RC=%s\n' "$amp_off_rc"
printf 'SPEAKER_MFP_MUTE_RC=%s\n' "$mfp_on_rc"

if [ "$play_rc" -ne 0 ] || [ "$amp_off_rc" -ne 0 ] || [ "$mfp_on_rc" -ne 0 ]; then
	exit 1
fi

printf '%s\n' 'SPEAKER_OUTPUT_HIGH_TONE_TEST_RESULT=PASS'
exit 0
