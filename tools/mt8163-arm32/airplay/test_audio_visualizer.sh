#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd -P)
WORK=$(mktemp -d /tmp/libreecho-audio-visualizer-test.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

${CC:-cc} -O2 -std=c99 -Wall -Wextra -Wpedantic -Werror \
  "$SCRIPT_DIR/audio_visualizer.c" \
  "$SCRIPT_DIR/test_audio_visualizer.c" \
  -lm -o "$WORK/test-audio-visualizer"
"$WORK/test-audio-visualizer"
