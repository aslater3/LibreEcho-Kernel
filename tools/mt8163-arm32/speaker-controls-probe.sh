#!/bin/sh

set -u

echo SPEAKER_CONTROLS_PROBE_START
id
uname -a

if [ ! -x /tmp/tinymix ]; then
	printf '%s\n' 'SPEAKER_CONTROLS_PROBE_ERROR=tinymix-missing'
	exit 1
fi

/tmp/tinymix -D 0 contents > /tmp/speaker-tinymix.contents
rc=$?

printf '%s\n' 'SPEAKER_CONTROLS_BEGIN'
grep -E -i 'amp|speaker|headset|mute|volume|gain|mux|clock' \
	/tmp/speaker-tinymix.contents || true
printf '%s\n' 'SPEAKER_CONTROLS_END'
printf 'SPEAKER_CONTROLS_TINYMIX_RC=%s\n' "$rc"

if [ "$rc" -ne 0 ]; then
	exit "$rc"
fi

printf '%s\n' 'SPEAKER_CONTROL_VALUES_BEGIN'
for control in 0 1 3 5 7 13 61 62 63 64 65 66 67 72 87 88; do
	printf 'CONTROL_ID=%s\n' "$control"
	/tmp/tinymix -D 0 get "$control" || exit 1
done
printf '%s\n' 'SPEAKER_CONTROL_VALUES_END'
printf '%s\n' 'SPEAKER_CONTROLS_PROBE_RESULT=PASS'
exit 0
