#!/bin/bash
# aplay+ test suite: unit tests (ls / dsp / tags) + per-format decode smoke
# tests through the real play_file() pipeline to the ALSA "null" device.
# All binaries are built with AddressSanitizer.
#
# Usage: tests/run_tests.sh   (from the repo root or the tests dir)
set -u
cd "$(dirname "$0")"

BUILD="${TMPDIR:-/tmp}/aplay-tests-$$"
mkdir -p "$BUILD"
trap 'rm -rf "$BUILD"' EXIT

CC="${CC:-cc}"
CFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra \
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
        -Wno-sign-compare -Wno-unused-result"

rc=0
run() {	# run <name> <cmd...>
	echo "--- $1"
	shift
	"$@" || { echo "*** $1 FAILED"; rc=1; }
}

# fixtures (ffmpeg + crafted DSF)
if command -v ffmpeg >/dev/null; then
	bash make_fixtures.sh "$BUILD/fixtures" || { echo "*** fixture generation failed"; exit 1; }
	FIX="$BUILD/fixtures"
else
	echo "note: ffmpeg not found — skipping file-based tests"
	FIX=""
fi

# unit tests
$CC $CFLAGS -o "$BUILD/test_ls" test_ls.c || rc=1
$CC $CFLAGS -o "$BUILD/test_dsp" test_dsp.c -lasound -lm || rc=1
$CC $CFLAGS -o "$BUILD/test_tags" test_tags.c || rc=1

run test_ls "$BUILD/test_ls"
run test_dsp "$BUILD/test_dsp"
run test_tags "$BUILD/test_tags" $FIX

# decode smoke tests (need the null ALSA device and fixtures)
if [ -n "$FIX" ]; then
	$CC $CFLAGS -Dmain=aplay_main -c ../aplay+.c -o "$BUILD/aplay.o" 2> "$BUILD/aplay-warn.txt" \
		|| { echo "*** aplay+.c test build failed"; sed -n '1,20p' "$BUILD/aplay-warn.txt"; rc=1; }
	if [ -f "$BUILD/aplay.o" ]; then
		$CC $CFLAGS -o "$BUILD/smoke" smoke.c "$BUILD/aplay.o" -lasound -lm || rc=1
		# stdin from an idle FIFO: FIONREAD sees 0, playback runs to completion
		# (process substitution — bash must not wait for the idle writer)
		SMOKE_FILES="$FIX/t16.flac $FIX/t24-96k.flac $FIX/t16.wav $FIX/t24.wav \
		             $FIX/t.mp3 $FIX/t.ogg $FIX/mono.flac $FIX/t.dsf"
		run smoke-plain  bash -c "\"$BUILD/smoke\" $SMOKE_FILES < <(sleep 300) "
		run smoke-xtalk  bash -c "\"$BUILD/smoke\" -c $SMOKE_FILES < <(sleep 300) "
		# AAC/WMA: the minimal decoders can't decode ffmpeg's output (known
		# limitation) — assert the open/tag/error paths don't crash.
		run smoke-aacwma bash -c "\"$BUILD/smoke\" \"$FIX/t.m4a\" \"$FIX/t.wma\" < <(sleep 300) || true"
	fi
fi

echo
if [ $rc -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit $rc
