#!/usr/bin/env bash
# Phase 68 test asset generator. Produces:
#   bounce.wav -- 440 Hz sine, 0.1 s, mono, 16-bit PCM, 22050 Hz (~4 KB)
#   loop.ogg   -- 220 Hz sine, 3 s, mono, 22050 Hz Vorbis (~15 KB)
#
# Usage:   ./tests/assets/generate-audio.sh
# Output:  tests/assets/bounce.wav + tests/assets/loop.ogg
#
# Sources are programmatically-generated sine waves -- CC0 / public-domain
# provenance is trivially satisfied.
#
# Tool choice: ffmpeg handles both .wav and .ogg encoding uniformly.
# The 68-RESEARCH.md recipe suggested sox for bounce.wav, but sox is not
# available on every macOS / Linux dev box; ffmpeg is. If sox IS
# available, the equivalent sox one-liner is:
#   sox -n -r 22050 -c 1 -b 16 bounce.wav synth 0.1 sine 440 vol 0.5

set -euo pipefail

cd "$(dirname "$0")"

# bounce.wav -- short 440 Hz (A4) ping, 0.1 s, 16-bit PCM mono @ 22050 Hz
ffmpeg -y -f lavfi -i "sine=frequency=440:duration=0.1" \
       -ac 1 -ar 22050 -sample_fmt s16 bounce.wav 2>/dev/null

# loop.ogg -- 3-second 220 Hz (A3) drone, Vorbis mono @ 22050 Hz
ffmpeg -y -f lavfi -i "sine=frequency=220:duration=3" \
       -ac 1 -ar 22050 loop.ogg 2>/dev/null

# Report sizes (portable stat: macOS uses -f, Linux uses -c)
bsz=$(stat -f %z bounce.wav 2>/dev/null || stat -c %s bounce.wav)
lsz=$(stat -f %z loop.ogg   2>/dev/null || stat -c %s loop.ogg)
echo "Generated: $(pwd)/bounce.wav (${bsz} bytes)"
echo "Generated: $(pwd)/loop.ogg (${lsz} bytes)"
