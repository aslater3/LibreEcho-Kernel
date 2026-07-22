#!/bin/sh

set -u

echo CODEC_ROUTE_PROBE_START
if [ ! -x /tmp/tinymix ]; then
	echo CODEC_ROUTE_PROBE_ERROR=tinymix-missing
	exit 1
fi

/tmp/tinymix -D 0 contents > /tmp/codec-tinymix.contents
rc=$?
printf 'CODEC_ROUTE_TINYMIX_RC=%s\n' "$rc"
grep -E -i 'R_DAC|L_DAC|HPR|HPL|LOL|LOR|Output Mixer|DAC Switch|I2S1|MFP|LineOut|DacMux' \
	/tmp/codec-tinymix.contents || true
echo CODEC_ROUTE_PROBE_RESULT=PASS
exit "$rc"
