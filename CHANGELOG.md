# Changelog

## 2026-07-03 — Code-review round: correctness, performance, structure

A full review of the fork's own code (`aplay+.c`, `alsa.h`, `browser.h`,
`ls.h`, `kbhit.h`, `Makefile`) followed by a fix round. Vendored decoders
were not touched. A test suite now lives in `tests/` (`make test`).

### Removed: `-V` software volume

`-V` never worked as advertised — the volume factor was only applied inside
the crosstalk-cancellation filter, so `-V 0.5` *without* `-c` played at full
volume while the indicator claimed `MODIFIED · software volume`. Rather than
wire volume into every play path, it was removed entirely: volume is the
DAC/amp's job in a bit-perfect player. `MODIFIED` now means exactly one
thing: crosstalk cancellation is altering samples.

### Fixed: crosstalk corrupted hi-res (S32) audio

`-c` treated every non-float buffer as 16-bit. Hi-res FLAC/WAV/DSF play as
32-bit (`S32_LE`), so crosstalk reinterpreted 32-bit samples as 16-bit —
loud garbage. The DSP (now in `dsp.h`) handles S16/S32/FLOAT natively and is
always given the format ALSA actually negotiated, not the one requested
(those differ when plughw falls back, e.g. FLOAT → S32). Unit-tested: the
S32 path matches the FLOAT path bit-for-bit within rounding, and state is
continuous across chunk boundaries.

### Fixed: the crosstalk *delay* was zero all along

Found by the new impulse-response test: the delay ring buffer is exactly
`delay_samples` frames long, and the code stored the current sample *before*
reading the "delayed" one — so it read its own write back. The "delayed,
attenuated opposite channel" was actually the *current* opposite channel:
plain instant crossfeed subtraction, no delay, ever since the feature was
written. The delayed sample is now read before the slot is overwritten, so
`-c` finally applies the speaker-distance delay (`-D`) it always claimed to.
**`-c` will sound different than before — that's the feature working for the
first time.** If you preferred the old sound, it was equivalent to `-D 0`
(nearly — a 1-frame delay is the minimum the ring supports).

### Fixed: MP3s decoded the *entire file* before playing

`play_mp3` called `drmp3_get_pcm_frame_count()` for the progress total,
which decodes the whole file up front, then rewinds and decodes it again.
MP3 tracks now start instantly; progress shows elapsed time (a total would
require that full decode — deliberately skipped).

### Fixed: Ogg Vorbis could silently drop audio

The decode call passed a buffer size of half the actual buffer, and both
were smaller than the Vorbis spec's maximum block (8192 samples/channel);
stb_vorbis silently discards samples that don't fit. The decode buffer is
now sized for a full spec-max frame.

### Fixed: pause on DACs without hardware pause

Space always called `snd_pcm_pause()`, which many USB DACs don't support —
pause silently did nothing there. Hardware pause is still used where
supported; elsewhere pause is emulated with `snd_pcm_drop` + re-prepare on
resume (drops the small buffered tail, but pause now works on every device).

### Fixed: underruns dropped a chunk of audio

On `-EPIPE` the player recovered the stream but threw away the frames that
failed to write. The chunk is now rewritten once after recovery. (The
message also said "overrun"; a playback EPIPE is an *under*run.)

### Fixed: `ls.h` rewritten (crash-class bugs, half the I/O)

The directory lister had an off-by-one that wrote entry metadata one slot
ahead of the name — the origin of the mysterious `+1` allocation, the empty
padding slots, and the defensive guards sprinkled through `play_dir`. It
also walked every directory twice (count, then fill), `chdir`'d around the
process, used unbounded `sprintf`, shuffled the empty slot into the list,
and fabricated a phantom entry for unreadable directories. Rewritten as a
single pass with a growing array, no `chdir`, `snprintf` throughout, a
correct Fisher-Yates shuffle over real entries only, and `d_type` instead
of a `stat` per entry where the filesystem provides it. `ls_dir` now has a
clean contract: files only, `*num` exact, no empty slots. `findExt` no
longer reads dots from the directory part of a path (`dir.name/file` used
to confuse it) and returns `""` for dot-less names.

### Fixed: regex filter recompiled (and leaked) per track

`play_dir -s <regex>` compiled the pattern for every file on every pass and
never freed it. Compiled once, freed at the end.

### Restructured: one playback driver instead of seven copies

The seven `play_*` functions shared the same skeleton (open → tags →
now-playing → ALSA init → crosstalk → decode/play/key/progress loop →
cleanup) with per-format drift — which is exactly where the crosstalk and
format bugs above lived. There is now a single `play_pcm()` driver fed by
small per-format read callbacks (`PCMSource`). Native-DSD passthrough and
test mode keep their own loops. Per-format behavior is unchanged; format
defaulting (S32 for >16-bit sources) is now in one place.

Also folded in:
- progress output throttled to once per second of audio (was ~200
  printfs/sec at 5ms periods) and shown as `elapsed / total` time;
- the supported-extension list now lives in ONE table driving both
  `play_file()` and the browser's file filter;
- `play_wma`'s five copy-pasted cleanup ladders → one `goto cleanup` path;
- dead code removed: the unreachable minimp3 fallback (`DR_MP3` is always
  defined), the unused `display_progress()`, and the never-used
  `MAX_DELAY_SAMPLES`;
- tag parsers extracted to `tags.h` (pure, unit-tested), DSP to `dsp.h`.

### Smaller fixes

- `select_alsa_device()` had a byte-for-byte copy of `read_menu_choice()`'s
  raw-input loop; it now calls it. The saved-device config is read once.
- `kbhit()` no longer flips terminal modes behind the restore logic's back
  (and no longer returns garbage when `FIONREAD` fails); raw mode is set
  explicitly for `-n`/`-T` playback, as the browser already did.
- Browser directory reads use `d_type` (no `stat` per entry on big dirs)
  and check `realloc` failures.
- `hw:`/`plughw:` string checks centralized in `dev_is_hw()` helpers.
- `-h` exits 0.
- `Makefile`: new `release` target (keeps the unstripped binary as
  `aplay+.debug` for `addr2line`, ships a stripped `aplay+`), new `test`
  target, and `-Wno-stringop-overflow` dropped (the warning it hid was the
  `ls.h` `sprintf`, now fixed).

### Tests (`tests/`, run with `make test`)

- `test_ls.c` — natural sort, `findExt`, and the full `ls_dir` contract
  (counts, ordering, recursion, shuffle-is-a-permutation, empty/missing
  dirs). The pre-rewrite `ls.h` fails 12 of these checks; the rewrite
  passes all.
- `test_dsp.c` — crosstalk: mono no-op, impulse response (delayed
  attenuated opposite-channel subtraction), silence stays silent,
  S32 ≡ FLOAT equivalence, chunked ≡ whole processing.
- `test_tags.c` — ID3v2/MP4/ASF/DSF parsers against ffmpeg-generated
  fixtures + a hand-crafted DSF; wrong-format and empty-file robustness.
- `smoke.c` — plays every fixture (16/24-bit FLAC, 16/24-bit WAV, MP3, Ogg,
  mono, DSF) through the real `play_file()` pipeline to the ALSA `null`
  device, with and without crosstalk, all under AddressSanitizer. AAC/WMA
  exercise the open/tag/error paths (the minimal decoders can't decode
  ffmpeg's encodings — pre-existing limitation).
