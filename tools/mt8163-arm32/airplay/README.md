# AirPlay 2 image inputs

The image packages AirPlay 2 support by default, but the runtime controller
leaves both processes stopped until the UI integration toggle is enabled.

Pinned upstream source inputs for the ARMHF build are:

- Shairport Sync 5.1, commit `d6ac53bf4c6a1ebc55a03177537765ff42dec919`
- NQPTP 1.2.8, commit `c925f27c1fd12e4033ac477e5a405969b0b0260b`

Shairport Sync must be configured with `--with-airplay-2`, the raw pipe backend, OpenSSL,
FFmpeg, libplist, libsodium, libgcrypt, UUID and Avahi. The Avahi runtime
closure also includes D-Bus and its glibc/systemd support libraries. NQPTP
must run before Shairport Sync when the integration is enabled. The pipeline
keeps the ARMHF dependency sysroot pinned and separate from the target's small
musl userspace: NQPTP is static ARM32, while Shairport Sync is ARM32
glibc-linked and ships with its audited loader/library closure. FFmpeg is built
as a small static audio-only subset so the image does not inherit the host's
full codec dependency tree.

The device's 3.18 ASoC driver is usable through TinyALSA but returns
`ENOTTY` for the libasound probing ioctls used by Shairport's ALSA backend.
The payload therefore uses Shairport's raw named-pipe backend. The
`libreecho-airplay-audio` process is now only a producer: it forwards decoded
S16_LE/48 kHz/stereo PCM to the shared media bus and never opens ALSA.
`libreecho-audio-engine` is the sole TinyALSA/codec/amplifier owner. It mixes
media, system, announcement, and alarm buses; ducks media by 12 dB under
higher-priority audio; averages the result to a mono speaker bus with
clipping-safe 32-bit arithmetic; and writes PCM `0,23`. The stock Puffin codec
profile then sends the left/HPL high-pass band to the tweeter and the
right/HPR low-pass band to the woofer. A linked peak limiter restores the
stock pipeline's +3 dB output trim without allowing PCM clipping or positive
codec gain. Active announcement audio also requests a slow green pulse from
the LED daemon. The request is best-effort and owner-scoped, so audio continues
if LED control is unavailable and the previous LED pattern is restored when
the announcement bus becomes idle.

Shairport's pipe must use `ignore_volume_control = "yes"` because the external
volume hook owns codec attenuation. Otherwise Shairport attenuates the PCM in
software and the hook applies the same AirPlay attenuation again.
The engine reapplies the physical amplifier controls after the codec starts
DMA. AirPlay dB callbacks update only the media-bus software gain, so an
announcement can remain audible above quiet media without changing the
device-wide codec volume.

The Avahi/D-Bus payload remains inside the fixed 16 MiB boot envelope by using
the free range below the DT-reserved RAM console at `0x44400000`.

The normal build prefers `/usr/bin/arm-linux-gnueabihf-g++` and falls back to
the ARMHF C driver when the host has no separate C++ driver; the pinned
AirPlay sources are C. CI or a release builder can override this explicitly
with `LIBREECHO_AIRPLAY_CXX`.
