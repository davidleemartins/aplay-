// ©2017-2025 Yuichiro Nakada
// Modifications © 2026 David Lee Martins
// clang -Os -o aplay+ aplay+.c -lasound

#include <stdio.h>
#include <sys/mman.h>
#include <string.h> // for memcpy
#include <math.h>   // for sin
#include <fcntl.h>  // for open, lseek
#include <unistd.h> // for close

// UI color theme (full ANSI escapes). Defined before alsa.h so AUDIO_init_auto's
// BIT PERFECT accent can use it. Built-in default below; overridable via a few
// presets and per-color keys in ~/.config/aplay+/theme (see theme_load()).
#define THEME_LEN 48	// room for truecolor SGR like "38;2;R;G;B;48;2;R;G;B"
typedef struct {
	char header[THEME_LEN], dir[THEME_LEN], file[THEME_LEN], sel[THEME_LEN],
	     footer[THEME_LEN], accent[THEME_LEN], playing[THEME_LEN], hint[THEME_LEN],
	     warn[THEME_LEN], alert[THEME_LEN];
} Theme;
Theme theme = {
	.header  = "\e[1;36m",
	.dir     = "\e[1;34m",
	.file    = "",
	.sel     = "\e[7m",
	.footer  = "\e[7m",
	.accent  = "\e[1;32m",       // BIT PERFECT (green)
	.playing = "\e[1;35m",
	.hint    = "\e[2m",
	.warn    = "\e[1;33m",       // MODIFIED / FORMAT-CONVERTED (yellow)
	.alert   = "\e[38;5;214m",   // RESAMPLED (amber)
};

// App-side DSP state, declared before alsa.h so AUDIO_init_auto's playback-status
// indicator can tell whether the output is still bit-perfect. (Note: AUDIO_init*'s
// own `flag` param is the stream direction, not these app flags.)
int crosstalk = 0;	// set when -c is given
int g_interrupt = 0;	// set when a keypress stops the current track, so AUDIO_close
			// drops the buffered audio (instant) instead of draining it
#define USE_FLOAT32   128
#define USE_CROSSTALK 256 // Flag for crosstalk cancellation
#define USE_TEST_MODE 512 // Flag for test mode

#include "alsa.h"
#include "dsp.h"	// crosstalk cancellation (needs SND_PCM_FORMAT_* from alsa.h)
#include "tags.h"	// ID3v2 / MP4 / ASF tag parsers + Tags + fmt_duration
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
#include "uaac.h"
#include "uwma.h"
#include "stb_vorbis.h"
#define DSD_DECODER_IMPLEMENTATION
#include "dsd.h"

#define PARG_IMPLEMENTATION
#include "parg.h"
#include "regexp.h"

#include "random.h"
#include "ls.h"
#include "kbhit.h"

int verbose = 0;
int loop_mode = 0;
int force_pcm_dsd = 0; // -P forces DSD->PCM conversion instead of native passthrough
float speaker_distance_m = 0.5;
int cmd;
int key(AUDIO *a)
{
	if (!kbhit()) {
		return 0;
	}

	int c = cmd = getchar();
	if (verbose) {
		printf("%x\n", c);
	}
	if (c==0x20) {
		// Pause. Many USB DACs don't support hardware pause — emulate it there
		// by dropping the buffered audio and re-preparing on resume (loses the
		// small buffered tail, but pause works on every device).
		if (a->can_pause) snd_pcm_pause(a->handle, 1);
		else              snd_pcm_drop(a->handle);
		do {
			usleep(1000); // us
		} while (!kbhit());
		getchar(); // clear
		cmd = 0;
		if (a->can_pause) snd_pcm_pause(a->handle, 0);
		snd_pcm_prepare(a->handle);
		return 0;
	}

	// Any key other than 'c' (crosstalk toggle) stops the current track — mark it
	// so AUDIO_close drops the buffer instead of draining (snappy skip/quit).
	if (c && c != 'c') g_interrupt = 1;

	return c;
}

char *dev = "hw:0,0";  // BitPerfect

#define FRAMES        32

void print_test_mode_status(int phase, int flag, CrosstalkCancel *xtc)
{
	printf("\r\e[K");

	if (phase == 0) {
		printf("Phase: Left channel   | ");
	} else if (phase == 1) {
		printf("Phase: Right channel  | ");
	} else {
		printf("Phase: Panning L->R | ");
	}

	if (flag & USE_CROSSTALK) {
		printf("Crosstalk: ON (%.2f) | ", xtc->attenuation);
	} else {
		printf("Crosstalk: OFF        | ");
	}

	printf("Keys: [c]toggle-xtalk, [+,-]adjust, [q]uit");
	fflush(stdout);
}

// ---- now-playing metadata block (Tags/parsers/fmt_duration live in tags.h) ----
// Print just the tag lines (title, then "artist — album"), when present.
static void print_tags(const Tags *t)
{
	if (!t) return;
	if (t->title[0])
		printf("  %s%s\e[0m\n", theme.playing, t->title);
	if (t->artist[0] || t->album[0])
		printf("  %s%s%s%s\e[0m\n", theme.hint,
		       t->artist[0] ? t->artist : "",
		       (t->artist[0] && t->album[0]) ? " \xe2\x80\x94 " : "",
		       t->album[0] ? t->album : "");
}

// Print a themed "now playing" block: tags (when available) + a technical line
// with codec, rate, bit depth, channels, duration and average bitrate.
static void print_now_playing(const char *codec, const Tags *t,
                              unsigned rate, int bits, int ch,
                              unsigned long total_frames, long filesize)
{
	double sec = rate ? (double)total_frames / rate : 0;

	print_tags(t);

	printf("  %s%s", theme.hint, codec);
	if (rate) printf(" \xc2\xb7 %u Hz", rate);
	if (bits) printf(" \xc2\xb7 %d-bit", bits);
	if (ch)   printf(" \xc2\xb7 %dch", ch);
	if (sec > 0) {
		char dur[16];
		fmt_duration(dur, sizeof dur, sec);
		printf(" \xc2\xb7 %s", dur);
		if (filesize > 0)
			printf(" \xc2\xb7 %ld kbps", (long)(filesize * 8.0 / sec / 1000.0));
	}
	printf("\e[0m\n");
}

void play_test_mode(int format, int flag)
{
	const int sample_rate = 44100; // Standard sample rate
	const int channels = 2;        // Stereo
	const double duration = 5.0;   // 5 seconds per phase
	const int frames_per_phase = (int)(sample_rate * duration);
	const int total_phases = 3;    // Left, Right, Pan
	const int frequency = 400;

	AUDIO a;
	if (AUDIO_init_auto(&a, dev, sample_rate, channels, FRAMES, 1, format)) {
		printf("Error: Failed to initialize ALSA for stereo output\n");
		return;
	}

	printf("Entering Test mode...\n");
	if (format == SND_PCM_FORMAT_FLOAT_LE) {
		printf("Format: 32-bit FLOAT\n");
	} else {
		printf("Format: 16-bit S16_LE\n");
	}

	CrosstalkCancel xtc;
	init_crosstalk_cancellation(&xtc, sample_rate, channels, speaker_distance_m);

	uint64_t global_sample_index = 0;
	int phase_sample_index = 0;
	int phase = 0;
	printf("\e[?25l");
	print_test_mode_status(phase, flag, &xtc);
	while (1) {
		int k = key(&a);
		if (k) {
			if (k == 'q' || k == 0x1b) {
				break;
			}
			if (k == 'c') {
				flag ^= USE_CROSSTALK;
			}
			if (k == '+' || k == '=') {
				xtc.attenuation += 0.05f;
				if (xtc.attenuation > 1.0f) {
					xtc.attenuation = 1.0f;
				}
			}
			if (k == '-') {
				xtc.attenuation -= 0.05f;
				if (xtc.attenuation < 0.0f) {
					xtc.attenuation = 0.0f;
				}
			}
			print_test_mode_status(phase, flag, &xtc);
		}

		void *output_buffer = a.buffer;
		float *buffer_f32 = (float*)a.buffer;
		int16_t *buffer_s16 = (int16_t*)a.buffer;
		for (int i = 0; i < FRAMES; i++) {
			double t = (double)(global_sample_index + i) / sample_rate;

			int current_phase_sample = phase_sample_index + i;
			double left_amplitude, right_amplitude;

			if (phase == 0) { // Left channel only
				left_amplitude = 0.5;
				right_amplitude = 0.0;
			} else if (phase == 1) { // Right channel only
				left_amplitude = 0.0;
				right_amplitude = 0.5;
			} else { // Pan from left to right
				double pan = (double)current_phase_sample / frames_per_phase;
				if (pan > 1.0) {
					pan = 1.0;
				}
				left_amplitude = 0.5 * (1.0 - pan);
				right_amplitude = 0.5 * pan;
			}

			double left = left_amplitude * sin(2.0 * M_PI * frequency * t);
			double right = right_amplitude * sin(2.0 * M_PI * frequency * t);

			if (format == SND_PCM_FORMAT_FLOAT_LE) {
				buffer_f32[i * 2] = (float)left;
				buffer_f32[i * 2 + 1] = (float)right;
			} else {
				buffer_s16[i * 2] = (int16_t)(left * 32767.0f);
				buffer_s16[i * 2 + 1] = (int16_t)(right * 32767.0f);
			}
		}

		if (flag & USE_CROSSTALK) {
			apply_crosstalk_cancellation(&xtc, output_buffer, FRAMES, channels, format);
		}

		AUDIO_play0(&a);
		AUDIO_wait(&a, 100);

		global_sample_index += FRAMES;
		phase_sample_index += FRAMES;
		if (phase_sample_index >= frames_per_phase) {
			phase_sample_index = 0;
			phase = (phase + 1) % total_phases;
			print_test_mode_status(phase, flag, &xtc);
		}
	}
	printf("\n\e[?25h");

	AUDIO_close(&a);
	free_crosstalk_cancellation(&xtc);
}

static long file_size(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0) ? (long)st.st_size : 0;
}

// ---- unified PCM playback driver ----------------------------------------
// Every decoder is wrapped in a PCMSource; play_pcm() owns the shared loop:
// format defaulting, ALSA negotiation, crosstalk at the NEGOTIATED format,
// throttled progress, key handling and cleanup. Native-DSD passthrough and
// test mode keep their own loops (no PCM conversion / no file).
typedef struct PCMSource PCMSource;
struct PCMSource {
	// Decode the next chunk in `out_format`. `drvbuf` (capacity `drvframes`
	// frames) may be filled, or the source may hand back its own buffer via
	// *out (sources with natural framing: AAC, WMA, Ogg). Returns the number
	// of frames decoded, 0 at end of stream or on error.
	size_t (*read)(PCMSource *s, void *drvbuf, size_t drvframes, void **out);
	void *ctx;             // decoder handle / per-format state
	unsigned rate;         // source sample rate
	int ch;                // channels
	int src_bits;          // source bit depth (0 = unknown; >16 defaults to S32)
	int out_format;        // set by the driver after ALSA negotiation
	uint64_t total_frames; // 0 = unknown (progress shows elapsed time only)
};

// Returns 0 on success, 1 if the audio device could not be initialized.
static int play_pcm(PCMSource *s, int format, int flag)
{
	// If the user didn't request a specific format (-f), pick the best match
	// for the source bit depth. S16_LE for 16-bit, S32_LE for 20/24/32-bit.
	// This matters for DACs whose USB endpoints only expose 24-bit at high
	// sample rates — sending S16_LE to a 24-bit endpoint produces silence.
	if (!format && s->src_bits > 16) {
		format = SND_PCM_FORMAT_S32_LE;
	}

	AUDIO a;
	if (AUDIO_init_auto(&a, dev, s->rate, s->ch, FRAMES, 1, format)) {
		return 1;
	}
	s->out_format = a.actual_format;	// decode in what ALSA actually agreed to

	CrosstalkCancel xtc;
	init_crosstalk_cancellation(&xtc, s->rate, s->ch, speaker_distance_m);

	printf("\e[?25l");
	uint64_t done = 0;
	unsigned last_sec = (unsigned)-1;
	char tot[16] = "";
	if (s->total_frames && s->rate)
		fmt_duration(tot, sizeof tot, (double)s->total_frames / s->rate);

	size_t n;
	void *buf;
	while ((n = s->read(s, a.buffer, a.frames, &buf)) > 0) {
		if (flag & USE_CROSSTALK) {
			apply_crosstalk_cancellation(&xtc, buf, n, s->ch, s->out_format);
		}
		AUDIO_play(&a, buf, n);

		int k = key(&a);
		if (k == 'c') {
			flag ^= USE_CROSSTALK;
		} else if (k) {
			break;
		}

		// Progress, throttled to once per second of audio (not once per
		// period — that was ~200 printfs/second).
		done += n;
		unsigned sec = s->rate ? (unsigned)(done / s->rate) : 0;
		if (sec != last_sec) {
			last_sec = sec;
			char cur[16];
			if (sec == 0) snprintf(cur, sizeof cur, "0:00");	// elapsed 0 is known, not "--:--"
			else          fmt_duration(cur, sizeof cur, (double)sec);
			if (tot[0]) printf("\r%s / %s\e[K", cur, tot);
			else        printf("\r%s\e[K", cur);
			fflush(stdout);
		}
	}
	printf("\e[?25h");

	AUDIO_close(&a);
	free_crosstalk_cancellation(&xtc);
	return 0;
}

// ---- WAV -----------------------------------------------------------------
static size_t wav_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	drwav *w = (drwav*)s->ctx;
	*out = drvbuf;
	switch (s->out_format) {
	case SND_PCM_FORMAT_FLOAT_LE: return (size_t)drwav_read_pcm_frames_f32(w, drvframes, (float*)drvbuf);
	case SND_PCM_FORMAT_S32_LE:   return (size_t)drwav_read_pcm_frames_s32(w, drvframes, (drwav_int32*)drvbuf);
	default:                      return (size_t)drwav_read_pcm_frames_s16(w, drvframes, (drwav_int16*)drvbuf);
	}
}

void play_wav(char *name, int format, int flag)
{
	drwav wav;
	if (!drwav_init_file(&wav, name, NULL)) {
		fprintf(stderr, "Failed to open WAV: %s\n", name);
		return;
	}
	Tags tags = {{0}};	// WAV rarely carries tags; show codec/rate/duration
	print_now_playing("WAV", &tags, wav.sampleRate, wav.bitsPerSample,
	                  wav.channels, (unsigned long)wav.totalPCMFrameCount, file_size(name));

	PCMSource s = {
		.read = wav_read_cb, .ctx = &wav,
		.rate = wav.sampleRate, .ch = wav.channels, .src_bits = wav.bitsPerSample,
		.total_frames = wav.totalPCMFrameCount,
	};
	play_pcm(&s, format, flag);
	drwav_uninit(&wav);
}

// ---- FLAC ----------------------------------------------------------------
// Pull ARTIST/ALBUM/TITLE out of a FLAC's Vorbis comment block.
static void flac_tag_cb(void *pUser, drflac_metadata *pMeta)
{
	if (pMeta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
	Tags *t = (Tags *)pUser;
	drflac_vorbis_comment_iterator it;
	drflac_init_vorbis_comment_iterator(&it,
	    pMeta->data.vorbis_comment.commentCount, pMeta->data.vorbis_comment.pComments);
	drflac_uint32 len;
	const char *c;
	while ((c = drflac_next_vorbis_comment(&it, &len)) != NULL) {
		if (len >= 7 && strncasecmp(c, "ARTIST=", 7) == 0)
			snprintf(t->artist, sizeof t->artist, "%.*s", (int)(len - 7), c + 7);
		else if (len >= 6 && strncasecmp(c, "ALBUM=", 6) == 0)
			snprintf(t->album, sizeof t->album, "%.*s", (int)(len - 6), c + 6);
		else if (len >= 6 && strncasecmp(c, "TITLE=", 6) == 0)
			snprintf(t->title, sizeof t->title, "%.*s", (int)(len - 6), c + 6);
	}
}

static size_t flac_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	drflac *f = (drflac*)s->ctx;
	*out = drvbuf;
	switch (s->out_format) {
	case SND_PCM_FORMAT_FLOAT_LE: return (size_t)drflac_read_pcm_frames_f32(f, drvframes, (float*)drvbuf);
	case SND_PCM_FORMAT_S32_LE:   return (size_t)drflac_read_pcm_frames_s32(f, drvframes, (drflac_int32*)drvbuf);
	default:                      return (size_t)drflac_read_pcm_frames_s16(f, drvframes, (drflac_int16*)drvbuf);
	}
}

void play_flac(char *name, int format, int flag)
{
	Tags tags = {{0}};
	drflac *flac = drflac_open_file_with_metadata(name, flac_tag_cb, &tags, NULL);
	if (!flac) {
		fprintf(stderr, "Failed to open FLAC: %s\n", name);
		return;
	}
	print_now_playing("FLAC", &tags, flac->sampleRate, flac->bitsPerSample,
	                  flac->channels, (unsigned long)flac->totalPCMFrameCount, file_size(name));

	PCMSource s = {
		.read = flac_read_cb, .ctx = flac,
		.rate = flac->sampleRate, .ch = flac->channels, .src_bits = flac->bitsPerSample,
		.total_frames = flac->totalPCMFrameCount,
	};
	play_pcm(&s, format, flag);
	drflac_close(flac);
}

// ---- MP3 -----------------------------------------------------------------
static size_t mp3_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	drmp3 *m = (drmp3*)s->ctx;
	*out = drvbuf;
	if (s->out_format == SND_PCM_FORMAT_FLOAT_LE) {
		return (size_t)drmp3_read_pcm_frames_f32(m, drvframes, (float*)drvbuf);
	}
	if (s->out_format == SND_PCM_FORMAT_S32_LE) {
		// A FLOAT request that fell back to plughw's S32: decode S16 and widen
		// in place, back to front (each s32 write only clobbers already-read s16).
		size_t n = (size_t)drmp3_read_pcm_frames_s16(m, drvframes, (drmp3_int16*)drvbuf);
		int16_t *s16 = (int16_t*)drvbuf;
		int32_t *s32 = (int32_t*)drvbuf;
		for (long i = (long)(n * s->ch) - 1; i >= 0; i--) s32[i] = (int32_t)s16[i] << 16;
		return n;
	}
	return (size_t)drmp3_read_pcm_frames_s16(m, drvframes, (drmp3_int16*)drvbuf);
}

void play_mp3(char *name, int format, int flag)
{
	drmp3 mp3;
	if (!drmp3_init_file(&mp3, name, NULL)) {
		printf("Error: not a valid MP3 audio file!\n");
		return;
	}

	Tags tags = {{0}};
	read_id3v2(name, &tags);
	// No duration/total for MP3: counting frames (drmp3_get_pcm_frame_count)
	// decodes the entire file before playback starts. Progress shows elapsed.
	print_now_playing("MP3", &tags, mp3.sampleRate, 0, mp3.channels, 0, 0);

	PCMSource s = {
		.read = mp3_read_cb, .ctx = &mp3,
		.rate = mp3.sampleRate, .ch = mp3.channels,
	};
	play_pcm(&s, format, flag);
	drmp3_uninit(&mp3);
}

// ---- Ogg Vorbis ----------------------------------------------------------
typedef struct {
	stb_vorbis *v;
	short *buf;	// one full Vorbis frame; sized for the spec max block
	int num_shorts;
} OggSrc;

static size_t ogg_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	OggSrc *o = (OggSrc*)s->ctx;
	int n = stb_vorbis_get_frame_short_interleaved(o->v, s->ch, o->buf, o->num_shorts);
	if (n <= 0) return 0;
	*out = o->buf;
	return (size_t)n;
}

void play_ogg(char *name, int format, int flag)
{
	int error;
	stb_vorbis *v = stb_vorbis_open_filename(name, &error, NULL);
	if (!v) {
		printf("Error: cannot open `%s`\n", name);
		return;
	}
	Tags tags = {{0}};
	stb_vorbis_comment vc = stb_vorbis_get_comment(v);
	for (int i = 0; i < vc.comment_list_length; i++)
		if (vc.comment_list[i]) tag_set_kv(&tags, vc.comment_list[i]);
	print_now_playing("Ogg Vorbis", &tags, v->sample_rate, 0, v->channels,
	                  (unsigned long)stb_vorbis_stream_length_in_samples(v), file_size(name));

	// Spec-max Vorbis block is 8192 samples/channel; size for a whole frame so
	// stb_vorbis never has to truncate (it silently drops what doesn't fit).
	OggSrc o = { .v = v, .num_shorts = 8192 * v->channels };
	o.buf = (short*)malloc(o.num_shorts * sizeof(short));
	if (!o.buf) { stb_vorbis_close(v); return; }

	PCMSource s = {
		.read = ogg_read_cb, .ctx = &o,
		.rate = v->sample_rate, .ch = v->channels,
		.total_frames = stb_vorbis_stream_length_in_samples(v),
	};
	play_pcm(&s, 0, flag);	// stb_vorbis decodes S16 only
	free(o.buf);
	stb_vorbis_close(v);
}

// ---- WMA -----------------------------------------------------------------
#define MAX_WMA_FRAME_LEN 4096
#define MAX_WMA_SUBFRAMES 16
#define MAX_PCM_BUFFER_SIZE (MAX_WMA_SUBFRAMES * MAX_WMA_FRAME_LEN * 8 * sizeof(short))  // 8 channels max, ~1MB safe

typedef struct {
	CodecContext *cc;
	unsigned char *stream_pos;
	int bytes_left;
	short *sample_buf;
	size_t pcm_buffer_size;
} WmaSrc;

static size_t wma_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	WmaSrc *w = (WmaSrc*)s->ctx;
	if (w->bytes_left <= 0) return 0;

	int out_size = 0;
	int frame_size = wma_decode_superframe(w->cc, w->sample_buf, &out_size,
	                                       w->stream_pos, w->bytes_left);
	if (frame_size <= 0 || out_size <= 0) {
		fprintf(stderr, "Error: failed to decode WMA frame, frame_size=%d, out_size=%d, bytes_left=%d\n",
		        frame_size, out_size, w->bytes_left);
		return 0;
	}
	if (out_size > (int)w->pcm_buffer_size) {
		fprintf(stderr, "Error: decoded output size %d exceeds buffer size %zu\n",
		        out_size, w->pcm_buffer_size);
		return 0;
	}
	int frames = out_size / (sizeof(short) * s->ch);
	if (frames <= 0 || frames > MAX_WMA_FRAME_LEN * MAX_WMA_SUBFRAMES) {
		fprintf(stderr, "Error: invalid frame count %d\n", frames);
		return 0;
	}
	w->stream_pos += frame_size;
	w->bytes_left -= frame_size;
	*out = w->sample_buf;
	return (size_t)frames;
}

void play_wma(char *name, int format, int flag)
{
	void *file_data = MAP_FAILED;
	short *sample_buf = NULL;
	CodecContext cc = {0};
	int len = 0;

	int fd = open(name, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return;
	}
	len = lseek(fd, 0, SEEK_END);
	if (len <= 0) {
		fprintf(stderr, "Error: invalid file size\n");
		close(fd);
		return;
	}
	lseek(fd, 0, SEEK_SET);
	file_data = mmap(0, len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file_data == MAP_FAILED) {
		perror("mmap");
		return;
	}

	cc.priv_data = calloc(1, sizeof(WMADecodeContext));
	if (!cc.priv_data) {
		fprintf(stderr, "Error: failed to allocate WMA context\n");
		goto cleanup;
	}
	if (parse_wma_header((unsigned char*)file_data, len, &cc) != 0) {
		fprintf(stderr, "Error: failed to parse WMA header\n");
		goto cleanup;
	}
	WMADecodeContext *wctx = (WMADecodeContext *)cc.priv_data;
	if (wma_decode_init_fixed(&cc) < 0) {
		fprintf(stderr, "Error: failed to initialize WMA decoder\n");
		goto cleanup;
	}

	size_t pcm_buffer_size = wctx->nb_channels * wctx->frame_len * MAX_WMA_SUBFRAMES * sizeof(short);
	if (pcm_buffer_size == 0 || pcm_buffer_size > MAX_PCM_BUFFER_SIZE ||
	    wctx->nb_channels == 0 || wctx->frame_len == 0) {
		fprintf(stderr, "Error: invalid PCM buffer parameters: nb_channels=%d, frame_len=%d, pcm_buffer_size=%zu\n",
		        wctx->nb_channels, wctx->frame_len, pcm_buffer_size);
		goto cleanup_decoder;
	}
	sample_buf = malloc(pcm_buffer_size);
	if (!sample_buf) {
		fprintf(stderr, "Error: failed to allocate PCM buffer of size %zu bytes\n", pcm_buffer_size);
		goto cleanup_decoder;
	}

	Tags tags = {{0}};
	read_asf_tags(name, &tags);
	print_now_playing("WMA", &tags, cc.sample_rate, 0, cc.channels, 0, 0);

	{
		WmaSrc w = {
			.cc = &cc, .stream_pos = (unsigned char*)file_data, .bytes_left = len,
			.sample_buf = sample_buf, .pcm_buffer_size = pcm_buffer_size,
		};
		PCMSource s = {
			.read = wma_read_cb, .ctx = &w,
			.rate = cc.sample_rate, .ch = cc.channels,
		};
		play_pcm(&s, 0, flag);	// WMA decoder produces S16 only
	}

cleanup_decoder:
	wma_decode_end(&cc);
cleanup:
	free(sample_buf);
	free(cc.priv_data);
	if (file_data != MAP_FAILED) munmap(file_data, len);
}

// ---- AAC / M4A -----------------------------------------------------------
typedef struct {
	HAACDecoder aac;
	unsigned char *file_data, *stream_pos;
	int bytes_left;
	int samples_per_frame;	// per channel: 2048 with SBR, else 1024
	short buf[AAC_BUF_SIZE * 2];
} AacSrc;

static size_t aac_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	AacSrc *c = (AacSrc*)s->ctx;
	if (c->bytes_left <= 0) return 0;
	int r = AACDecode(c->aac, &c->stream_pos, &c->bytes_left, c->buf);
	if (r) {
		printf("\nAAC decode error %d\n", r);
		// For raw AAC (MP4), do not attempt ADTS-based resync — just stop.
		return 0;
	}
	*out = c->buf;
	return (size_t)c->samples_per_frame;
}

void play_aac(char *name, int format, int flag)
{
	int fd = open(name, O_RDONLY);
	if (fd < 0) {
		printf("Error: cannot open `%s`\n", name);
		return;
	}

	AacSrc *c = calloc(1, sizeof(AacSrc));
	if (!c) { close(fd); return; }

	int samplerate, channels, profile;
	c->file_data = uaac_extract_aac(fd, &c->bytes_left, &samplerate, &channels, &profile);
	close(fd);
	if (!c->file_data) {
		printf("Error: cannot read AAC data\n");
		free(c);
		return;
	}
	c->stream_pos = c->file_data;

	Tags tags = {{0}};
	read_mp4_tags(name, &tags);	// m4a/mp4 container tags (raw .aac has none)
	print_tags(&tags);

	AACFrameInfo info;
	memset(&info, 0, sizeof(AACFrameInfo));
	info.nChans = channels;
	info.sampRateCore = samplerate;
	info.profile = profile;

	c->aac = AACInitDecoder();
	AACSetRawBlockParams(c->aac, 0, &info);

	int output_samplerate = samplerate;
	int sbr_enabled = (profile == 5);
	if (sbr_enabled) {
		output_samplerate *= 2;
	} else if (samplerate <= 24000) {
		sbr_enabled = 1;
		info.profile = 5;
		output_samplerate *= 2;
		printf("Assuming potential HE-AAC with SBR, output samplerate: %dHz\n", output_samplerate);
	}
	c->samples_per_frame = sbr_enabled ? 2048 : 1024;
	printf("%dHz (output: %dHz) %dch\n", info.sampRateCore, output_samplerate, info.nChans);

	PCMSource s = {
		.read = aac_read_cb, .ctx = c,
		.rate = output_samplerate, .ch = info.nChans,
	};
	play_pcm(&s, 0, flag);	// helix decoder produces S16 only

	AACFreeDecoder(c->aac);
	free(c->file_data);
	free(c);
}

// ---- DSF (DSD) -----------------------------------------------------------
static size_t dsf_read_cb(PCMSource *s, void *drvbuf, size_t drvframes, void **out)
{
	DSDDecoder *d = (DSDDecoder*)s->ctx;
	*out = drvbuf;
	return dsd_decoder_read_pcm_frames(d, drvframes, drvbuf, s->out_format);
}

void play_dsf(char *name, int format, int flag)
{
	FILE *f = fopen(name, "rb");
	if (!f) {
		printf("Error: cannot open `%s`\n", name);
		return;
	}

	// Initialize DSD decoder from file stream
	DSDDecoder *decoder = dsd_decoder_init_file(f);
	if (!decoder) {
		printf("Error: failed to initialize DSD decoder for `%s`\n", name);
		fclose(f);
		return;
	}

	Tags tags = {{0}};
	read_dsf_tags(name, &tags);
	print_tags(&tags);	// title/artist/album above the DSD technical line

	// --- Try native DSD passthrough first (bit-perfect, no PCM conversion) ---
	// Only attempt on hw:/plughw: devices and when the user hasn't forced PCM.
	unsigned int native_rate = dsd_native_rate(decoder);
	if (dev_is_hw_or_plug(dev) && !force_pcm_dsd &&
	    AUDIO_supports_dsd(dev, SND_PCM_FORMAT_DSD_U32_BE, native_rate, decoder->channels)) {

		printf("%dHz DSD %dch (native DSD_U32_BE @ %uHz)\n",
		       decoder->sample_rate_dsd, decoder->channels, native_rate);

		AUDIO a;
		// Scale the period to at least ~5ms like AUDIO_init_auto does. A 32-frame
		// period at native DSD rates (176400Hz) is only 181µs — barely above the
		// USB 125µs packet interval — causing continuous underruns and silence.
		int native_frames = FRAMES;
		unsigned int min_frames = native_rate / 200; // 5ms worth
		while ((unsigned int)native_frames < min_frames) native_frames *= 2;

		if (AUDIO_init(&a, dev, native_rate, decoder->channels, native_frames,
		               1, SND_PCM_FORMAT_DSD_U32_BE)) {
			dsd_decoder_free(decoder);
			fclose(f);
			return;
		}

		// Native DSD passthrough is bit-perfect by definition (1-bit stream sent
		// straight to the DAC; no conversion, and crosstalk doesn't apply).
		printf("%s\xe2\x97\x8f BIT PERFECT\e[0m %s(native DSD)\e[0m\n", theme.accent, theme.hint);
		fflush(stdout);

		uint64_t total_native = dsd_native_total_frames(decoder);
		uint64_t frames_played = 0;
		unsigned last_sec = (unsigned)-1;
		char cur[16], tot[16];
		fmt_duration(tot, sizeof tot, native_rate ? (double)total_native / native_rate : 0);
		dsd_native_reset(decoder); // ensure clean start at data chunk
		printf("\e[?25l");
		size_t n;
		while ((n = dsd_read_native_u32be(decoder, a.frames, a.buffer)) > 0) {
			AUDIO_play(&a, a.buffer, n);
			int k = key(&a);
			if (k) break; // no crosstalk in native mode — it's a raw bitstream
			frames_played += n;
			unsigned sec = native_rate ? (unsigned)(frames_played / native_rate) : 0;
			if (sec != last_sec) {
				last_sec = sec;
				fmt_duration(cur, sizeof cur, (double)sec);
				printf("\r%s / %s\e[K", cur, tot);
				fflush(stdout);
			}
		}
		printf("\e[?25h");

		AUDIO_close(&a);
		dsd_decoder_free(decoder);
		fclose(f);
		return;
	}

	// --- Fallback: convert DSD to PCM ---
	printf("%dHz %dch (DSD->PCM)\n", decoder->sample_rate_pcm, decoder->channels);

	PCMSource s = {
		.read = dsf_read_cb, .ctx = decoder,
		.rate = decoder->sample_rate_pcm, .ch = decoder->channels,
		.src_bits = 24,	// DSF is always high-resolution — default to S32_LE
		.total_frames = decoder->totalPCMFrameCount,
	};
	play_pcm(&s, format, flag);

	dsd_decoder_free(decoder);
	fclose(f);
}

// ---- supported formats: ONE table drives play_file and the browser filter ----
static const struct {
	const char *ext;
	void (*fn)(char *name, int format, int flag);
} PLAYERS[] = {
	{ "flac", play_flac }, { "mp3", play_mp3 }, { "mp4", play_aac },
	{ "m4a", play_aac },   { "ogg", play_ogg }, { "wav", play_wav },
	{ "wma", play_wma },   { "dsf", play_dsf },
};
#define N_PLAYERS ((int)(sizeof(PLAYERS) / sizeof(PLAYERS[0])))

// Is this (lowercased) extension a playable audio format?
static int is_supported_audio(const char *ext)
{
	for (int i = 0; i < N_PLAYERS; i++)
		if (!strcasecmp(ext, PLAYERS[i].ext)) return 1;
	return 0;
}

// Play a single file by extension. Returns 1 if the file was recognised and
// played, 0 if the extension is unsupported (so callers can skip/continue).
int play_file(char *path, int format, int flag)
{
	char *e = findExt(path);
	for (int i = 0; i < N_PLAYERS; i++) {
		if (!strcasecmp(e, PLAYERS[i].ext)) {
			// Announce playback (and key hints) only for files we can play —
			// stay silent for cover art and other non-audio directory entries.
			printf("%s[ p prev  n next  space pause  q quit ]\e[0m\n", theme.hint);
			PLAYERS[i].fn(path, format, flag);
			return 1;
		}
	}
	return 0;
}

// Play a directory. If `start` is non-NULL, the first pass begins at the track
// whose basename matches it (so prev/next then navigate the rest of the album);
// later loop_mode passes play the whole directory.
void play_dir(char *name, char *type, char *regexp, int flag, const char *start)
{
	char path[PATH_MAX];
	int num=0, back=-1;	// so prev on the very first track replays it, not advances
	int format = flag & USE_FLOAT32 ? SND_PCM_FORMAT_FLOAT_LE : 0;

	// Compile the filter regex ONCE (it used to be recompiled — and leaked —
	// for every track on every pass).
	Reprog *prog = NULL;
	if (regexp) {
		const char *error;
		prog = regcomp(regexp, REG_ICASE, &error);
		if (!prog) {
			fprintf(stderr, "regcomp: %s\n", error);
			return;
		}
	}

	do {
		LS_LIST *ls = ls_dir(name, flag, &num);
		if (!ls) break;	// no listing (empty/alloc failure) — nothing to play
		int played_count = 0;
		int started = (start == NULL);	// skip tracks until we reach `start`
		for (int i=0; i<num; i++) {
			char *e = findExt(ls[i].d_name);
			if (type) {
				if (strcasecmp(e, type)) {
					continue;
				}
			}
			if (prog) {
				Resub m;
				if (regexec(prog, ls[i].d_name, &m, 0)) {
					continue;
				}
			}

			// On the first pass, skip everything before the chosen start track.
			if (!started) {
				const char *base = strrchr(ls[i].d_name, '/');
				base = base ? base + 1 : ls[i].d_name;
				if (strcmp(base, start) != 0) continue;
				started = 1;	// found it — play from here on
				back = i - 1;	// so prev on the start track replays it, not advances
			}

			printf("%s%s%s\e[0m\n", played_count > 0 ? "\n" : "", theme.playing, ls[i].d_name);
			snprintf(path, sizeof(path), "%s", ls[i].d_name);
			if (access(ls[i].d_name, F_OK)<0) {
				continue;
			}

			struct stat file_stat;
			if (stat(ls[i].d_name, &file_stat) < 0) {
				perror("stat");
				continue;
			}
			if (file_stat.st_size == 0) {
				printf("File size is 0: %s\n", ls[i].d_name);
				continue;
			}

			if (!play_file(path, format, flag)) {
				continue;
			}
			played_count++;

			if (cmd=='\\' || cmd=='p' || cmd=='b') {
				i = back;
			}
			if (cmd=='q' || cmd==0x1b) {
				break;
			}
			back = i-1;
		}
		free(ls);
		start = NULL;	// subsequent loop_mode passes play the whole directory
	} while (loop_mode);

	if (prog) regfree(prog);
}

// Play an .m3u/.m3u8 playlist: one file path per line, '#' lines are comments.
// Entries are resolved relative to the playlist's directory; Windows-style
// backslashes are accepted. prev/next/quit behave as in play_dir.
void play_m3u(const char *m3u, int flag)
{
	FILE *fp = fopen(m3u, "r");
	if (!fp) { fprintf(stderr, "cannot open playlist: %s\n", m3u); return; }

	// Directory of the playlist, used to resolve relative entries.
	char base[PATH_MAX];
	snprintf(base, sizeof(base), "%s", m3u);
	char *slash = strrchr(base, '/');
	if (slash) *slash = 0; else snprintf(base, sizeof(base), ".");

	char **paths = NULL;
	int n = 0, cap = 0;
	char line[PATH_MAX];
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = 0;	// strip CR/LF
		if (line[0] == 0 || line[0] == '#') continue;	// blank / comment / #EXTINF
		for (char *p = line; *p; p++) if (*p == '\\') *p = '/';	// Windows -> Unix

		char full[PATH_MAX];
		if (line[0] == '/') snprintf(full, sizeof(full), "%s", line);
		else                snprintf(full, sizeof(full), "%s/%s", base, line);

		if (n == cap) { cap = cap ? cap * 2 : 32; paths = realloc(paths, cap * sizeof(char*)); }
		paths[n++] = strdup(full);
	}
	fclose(fp);

	if (n == 0) { free(paths); fprintf(stderr, "playlist is empty: %s\n", m3u); return; }

	int format = flag & USE_FLOAT32 ? SND_PCM_FORMAT_FLOAT_LE : 0;
	int back = -1, played = 0, quit = 0;
	do {
		for (int i = 0; i < n && !quit; i++) {
			if (i < 0) continue;
			printf("%s%s%s\e[0m\n", played > 0 ? "\n" : "", theme.playing, paths[i]);
			if (access(paths[i], F_OK) < 0) { printf("  (missing)\n"); continue; }
			if (!play_file(paths[i], format, flag)) continue;
			played++;
			if (cmd == '\\' || cmd == 'p' || cmd == 'b') i = back;
			if (cmd == 'q' || cmd == 0x1b) { quit = 1; break; }
			back = i - 1;
		}
	} while (loop_mode && !quit);

	for (int i = 0; i < n; i++) free(paths[i]);
	free(paths);
}

#include <sched.h>
void set_realtime_priority()
{
	struct sched_param param;
	param.sched_priority = sched_get_priority_max(SCHED_FIFO);
	if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
		perror("Failed to set real-time priority");
	}
}
void set_cpu(char *c)
{
	char buff[256];
	for (int i=0; i<256; i++) {
		snprintf(buff, 255, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
		FILE *fp = fopen(buff, "w");
		if (!fp) {
			continue;
		}
		fprintf(fp, "%s", c);
		fclose(fp);
	}
}

typedef struct {
	char dev[32];	// "hw:X,Y"
	char name[64];
	char rates[64];
} DEVINFO;

// Enumerate hardware playback devices into `out` (up to `max`); returns the count.
int enumerate_alsa_devices(DEVINFO *out, int max)
{
	snd_ctl_t *ctl;
	snd_ctl_card_info_t *info;
	snd_pcm_info_t *pcminfo;
	snd_ctl_card_info_alloca(&info);
	snd_pcm_info_alloca(&pcminfo);

	int n = 0;
	int card = -1;
	while (snd_card_next(&card) >= 0 && card >= 0) {
		char ctlname[32];
		snprintf(ctlname, sizeof(ctlname), "hw:%d", card);
		if (snd_ctl_open(&ctl, ctlname, 0) < 0) continue;
		snd_ctl_card_info(ctl, info);

		int dev = -1;
		while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
			if (n >= max) break;
			snd_pcm_info_set_device(pcminfo, dev);
			snd_pcm_info_set_subdevice(pcminfo, 0);
			snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
			if (snd_ctl_pcm_info(ctl, pcminfo) < 0) continue;

			snprintf(out[n].dev, sizeof(out[n].dev), "hw:%d,%d", card, dev);
			snprintf(out[n].name, sizeof(out[n].name), "%.63s", snd_pcm_info_get_name(pcminfo));
			snprintf(out[n].rates, sizeof(out[n].rates), "?");

			// Get supported rates by opening the device directly
			snd_pcm_t *pcm;
			if (snd_pcm_open(&pcm, out[n].dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) >= 0) {
				snd_pcm_hw_params_t *params;
				snd_pcm_hw_params_alloca(&params);
				if (snd_pcm_hw_params_any(pcm, params) >= 0) {
					unsigned int common[] = { 44100, 48000, 88200, 96000, 176400, 192000, 0 };
					char *p = out[n].rates;
					char *end = out[n].rates + sizeof(out[n].rates) - 1;
					int first = 1;
					for (int i = 0; common[i]; i++) {
						if (snd_pcm_hw_params_test_rate(pcm, params, common[i], 0) == 0) {
							int written = snprintf(p, end - p, "%s%u",
							    first ? "" : " ", common[i]/1000);
							if (written > 0) { p += written; first = 0; }
						}
					}
					if (!first) snprintf(p, end - p, "kHz");
					else snprintf(out[n].rates, sizeof(out[n].rates), "unknown");
				}
				snd_pcm_close(pcm);
			}
			n++;
		}
		snd_ctl_close(ctl);
	}
	return n;
}

void list_alsa_devices()
{
	DEVINFO devs[64];
	int n = enumerate_alsa_devices(devs, 64);

	fprintf(stderr, "Available playback devices:\n\n");
	fprintf(stderr, "  %-14s %-28s %s\n", "Device", "Name", "Rates");
	fprintf(stderr, "  %-14s %-28s %s\n", "------", "----", "-----");
	for (int i = 0; i < n; i++) {
		fprintf(stderr, "  %-14s %-28.28s %s\n", devs[i].dev, devs[i].name, devs[i].rates);
	}
	fprintf(stderr, "\n");
}

// ---- persistent device choice (~/.config/aplay+/device) ----
void config_dir_path(char *out, size_t n)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0]) snprintf(out, n, "%s/aplay+", xdg);
	else              snprintf(out, n, "%s/.config/aplay+", home ? home : ".");
}

int config_load_device(char *out, size_t n)
{
	char dir[PATH_MAX], path[PATH_MAX];
	config_dir_path(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/device", dir);
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	int ok = (fgets(out, n, fp) != NULL);
	fclose(fp);
	if (!ok) return 0;
	out[strcspn(out, "\r\n")] = 0;	// strip trailing newline
	return out[0] ? 1 : 0;
}

// Best-effort recursive mkdir (like `mkdir -p`).
void mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
	}
	mkdir(tmp, 0755);
}

void config_save_device(const char *dev)
{
	char dir[PATH_MAX], path[PATH_MAX];
	config_dir_path(dir, sizeof(dir));
	mkdir_p(dir);
	snprintf(path, sizeof(path), "%s/device", dir);

	FILE *fp = fopen(path, "w");
	if (!fp) {
		fprintf(stderr, "warning: could not save device choice to %s\n", path);
		return;
	}
	fprintf(fp, "%s\n", dev);
	fclose(fp);
	fprintf(stderr, "saved device '%s' to %s\n", dev, path);
}

// ---- color theme: built-in presets + ~/.config/aplay+/theme overrides ----
static void theme_set(char *dst, const char *sgr)
{
	if (!sgr || !*sgr) { dst[0] = 0; return; }	// empty = no color
	snprintf(dst, THEME_LEN, "\e[%sm", sgr);
}

// Built-in theme names (also used by the in-browser theme picker).
static const char *theme_names[] = { "default", "mono", "contrast", "dracula" };
#define N_THEMES ((int)(sizeof(theme_names) / sizeof(theme_names[0])))

static void theme_preset(const char *name)
{
	if (!strcmp(name, "mono")) {
		theme_set(theme.header, "");     theme_set(theme.dir, "");      theme_set(theme.file, "");
		theme_set(theme.sel, "7");       theme_set(theme.footer, "7");  theme_set(theme.accent, "1");
		theme_set(theme.playing, "1");   theme_set(theme.hint, "2");
		theme_set(theme.warn, "1");      theme_set(theme.alert, "1");
	} else if (!strcmp(name, "contrast")) {
		theme_set(theme.header, "1;97"); theme_set(theme.dir, "1;93");  theme_set(theme.file, "");
		theme_set(theme.sel, "7");       theme_set(theme.footer, "1;7");theme_set(theme.accent, "1;92");
		theme_set(theme.playing, "1;95");theme_set(theme.hint, "2");
		theme_set(theme.warn, "1;93");   theme_set(theme.alert, "1;38;5;208");
	} else if (!strcmp(name, "dracula")) {
		// Dracula palette (truecolor): cyan/purple/pink/green/yellow/orange/comment.
		theme_set(theme.header,  "1;38;2;139;233;253");                 // cyan
		theme_set(theme.dir,     "1;38;2;189;147;249");                 // purple
		theme_set(theme.file,    "38;2;248;248;242");                   // foreground
		theme_set(theme.sel,     "38;2;248;248;242;48;2;68;71;90");     // fg on current-line bg
		theme_set(theme.footer,  "38;2;98;114;164;48;2;68;71;90");      // comment on bg
		theme_set(theme.accent,  "1;38;2;80;250;123");                  // green (BIT PERFECT)
		theme_set(theme.playing, "1;38;2;255;121;198");                 // pink (now playing)
		theme_set(theme.hint,    "38;2;98;114;164");                    // comment (dim)
		theme_set(theme.warn,    "1;38;2;241;250;140");                 // yellow
		theme_set(theme.alert,   "1;38;2;255;184;108");                 // orange (amber)
	} else { // "default"
		theme_set(theme.header, "1;36"); theme_set(theme.dir, "1;34");  theme_set(theme.file, "");
		theme_set(theme.sel, "7");       theme_set(theme.footer, "7");  theme_set(theme.accent, "1;32");
		theme_set(theme.playing, "1;35");theme_set(theme.hint, "2");
		theme_set(theme.warn, "1;33");   theme_set(theme.alert, "38;5;214");
	}
}

static char *theme_field(const char *key)
{
	if (!strcmp(key, "header"))  return theme.header;
	if (!strcmp(key, "dir"))     return theme.dir;
	if (!strcmp(key, "file"))    return theme.file;
	if (!strcmp(key, "sel"))     return theme.sel;
	if (!strcmp(key, "footer"))  return theme.footer;
	if (!strcmp(key, "accent"))  return theme.accent;
	if (!strcmp(key, "playing")) return theme.playing;
	if (!strcmp(key, "hint"))    return theme.hint;
	if (!strcmp(key, "warn"))    return theme.warn;
	if (!strcmp(key, "alert"))   return theme.alert;
	return NULL;
}

// Load ~/.config/aplay+/theme: a "theme=NAME" preset line plus optional
// "<field>=<SGR>" overrides (e.g. "dir=1;34", "accent=1;92", empty = no color).
void theme_load(void)
{
	char dir[PATH_MAX], path[PATH_MAX];
	config_dir_path(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/theme", dir);
	FILE *fp = fopen(path, "r");
	if (!fp) return;	// keep the compiled-in default

	char line[128];
	while (fgets(line, sizeof(line), fp)) {	// pass 1: preset
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "theme=", 6)) theme_preset(line + 6);
	}
	rewind(fp);
	while (fgets(line, sizeof(line), fp)) {	// pass 2: per-field overrides
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || line[0] == 0) continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		if (strcmp(line, "theme") == 0) continue;
		char *field = theme_field(line);
		if (field) theme_set(field, eq + 1);
	}
	fclose(fp);
}

int config_load_theme_name(char *out, size_t n)
{
	char dir[PATH_MAX], path[PATH_MAX];
	config_dir_path(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/theme", dir);
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	char line[128];
	int found = 0;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "theme=", 6)) {
			snprintf(out, n, "%s", line + 6);
			found = 1;
		}
	}
	fclose(fp);
	return found;
}

void config_save_theme(const char *name)
{
	char dir[PATH_MAX], path[PATH_MAX];
	config_dir_path(dir, sizeof(dir));
	mkdir_p(dir);
	snprintf(path, sizeof(path), "%s/theme", dir);
	FILE *fp = fopen(path, "w");
	if (!fp) {
		fprintf(stderr, "warning: could not save theme to %s\n", path);
		return;
	}
	fprintf(fp, "theme=%s\n", name);
	fclose(fp);
	fprintf(stderr, "saved theme '%s' to %s\n", name, path);
}

// Read a numbered menu choice in raw, no-echo mode (digits echoed by us).
// Returns the 1-based choice, 0 to cancel (Esc/q), or -1 if out of range.
static int read_menu_choice(int count, int def)
{
	struct termios old, raw;
	tcgetattr(0, &old);
	raw = old;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);

	char buf[8];
	int len = 0, cancelled = 0;
	for (;;) {
		unsigned char ch;
		if (read(0, &ch, 1) <= 0) { cancelled = 1; break; }
		if (ch == '\r' || ch == '\n') break;
		if (ch == 'q' || ch == 'Q') { cancelled = 1; break; }
		if (ch == 27) {	// Esc / arrow lead byte — cancel and drain
			struct termios d = raw;
			d.c_cc[VMIN] = 0; d.c_cc[VTIME] = 1;
			tcsetattr(0, TCSANOW, &d);
			unsigned char j;
			while (read(0, &j, 1) > 0) {}
			cancelled = 1;
			break;
		}
		if ((ch == 127 || ch == 8) && len > 0) {
			len--; fprintf(stderr, "\b \b"); fflush(stderr); continue;
		}
		if (ch >= '0' && ch <= '9' && len < (int)sizeof(buf) - 1) {
			buf[len++] = (char)ch; fputc(ch, stderr); fflush(stderr);
		}
	}
	tcsetattr(0, TCSANOW, &old);
	fprintf(stderr, "\n");
	if (cancelled) return 0;
	buf[len] = 0;
	int c = (len == 0) ? def : atoi(buf);
	return (c < 1 || c > count) ? -1 : c;
}

// Interactive theme picker. Applies the chosen preset live, returns 1 on success.
int select_theme(char *out, size_t n)
{
	char current[64];
	int have_cur = config_load_theme_name(current, sizeof(current));

	fprintf(stderr, "\nSelect a color theme:\n\n");
	for (int i = 0; i < N_THEMES; i++) {
		int cur = have_cur && !strcmp(theme_names[i], current);
		fprintf(stderr, "  %2d) %-10s%s\n", i + 1, theme_names[i], cur ? "  \xe2\x86\x90 current" : "");
	}
	fprintf(stderr, "\nEnter number [1-%d], Esc or q to cancel: ", N_THEMES);
	fflush(stderr);

	int c = read_menu_choice(N_THEMES, 1);
	if (c <= 0) {
		if (c == 0) fprintf(stderr, "cancelled.\n");
		else        fprintf(stderr, "invalid selection.\n");
		return 0;
	}
	snprintf(out, n, "%s", theme_names[c - 1]);
	fprintf(stderr, "\e[H\e[2J");	// clear before resuming
	return 1;
}

// Interactive picker. Writes the chosen "hw:X,Y" to `out`; returns 1 on success.
int select_alsa_device(char *out, size_t n)
{
	DEVINFO devs[64];
	int count = enumerate_alsa_devices(devs, 64);
	if (count == 0) {
		fprintf(stderr, "error: no ALSA playback devices found.\n");
		return 0;
	}

	// Mark the currently-saved device, if any, and make it the Enter default.
	char current[64];
	int have_cur = config_load_device(current, sizeof(current));
	int current_idx = -1;
	if (have_cur) {
		for (int i = 0; i < count; i++) {
			if (!strcmp(devs[i].dev, current)) { current_idx = i; break; }
		}
	}
	int def = (current_idx >= 0) ? current_idx + 1 : 1;

	fprintf(stderr, "\nSelect a playback device:\n\n");
	for (int i = 0; i < count; i++) {
		fprintf(stderr, "  %2d) %-14s %-28.28s %s%s\n",
		        i + 1, devs[i].dev, devs[i].name, devs[i].rates,
		        (i == current_idx) ? "  \xe2\x86\x90 current" : "");
	}
	if (current_idx < 0 && have_cur) {
		fprintf(stderr, "\n  (saved device '%s' is not currently available)\n", current);
	}
	fprintf(stderr, "\nEnter number [1-%d], Enter for %d, Esc or q to cancel: ", count, def);
	fflush(stderr);

	int choice = read_menu_choice(count, def);
	if (choice == 0) {
		fprintf(stderr, "cancelled — keeping current device.\n");
		return 0;
	}
	if (choice < 0) {
		fprintf(stderr, "invalid selection.\n");
		return 0;
	}
	snprintf(out, n, "%s", devs[choice - 1].dev);
	fprintf(stderr, "\e[H\e[2J");	// clear the busy device list after choosing
	return 1;
}

#include "browser.h"

// Restore the terminal (and leave the alt-screen if active) on any exit — normal
// return, exit(), or a signal such as Ctrl-C during playback (where ISIG is on).
static struct termios g_term_orig;
static int g_term_saved = 0;

// Non-canonical, no-echo mode for direct (non-browser) playback so key()'s
// p/n/space/q register immediately. ISIG stays on so Ctrl-C works; the atexit
// handler restores the original state. (kbhit() no longer mutates termios.)
static void term_playback_mode(void)
{
	if (!g_term_saved) return;
	struct termios t = g_term_orig;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_lflag |= ISIG;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
}
static void term_cleanup(void)
{
	if (g_alt_active) (void)!write(1, "\e[?25h\e[?1049l", 14);
	if (g_term_saved) tcsetattr(0, TCSANOW, &g_term_orig);
}
static void term_on_signal(int sig)
{
	term_cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

// On a fatal fault, restore the terminal and dump a backtrace so crashes are
// diagnosable (build keeps symbols; resolve further with addr2line if needed).
#include <execinfo.h>
static void crash_handler(int sig)
{
	term_cleanup();
	fprintf(stderr, "\n*** aplay+ crashed: signal %d (%s) ***\n", sig, strsignal(sig));
	void *bt[64];
	int n = backtrace(bt, 64);
	backtrace_symbols_fd(bt, n, 2);
	signal(sig, SIG_DFL);
	raise(sig);
}

void usage(FILE* fp, int argc, char** argv)
{
	fprintf(fp,
	        "Usage: %s [options] [file-or-dir]\n\n"
	        "With no options, opens an interactive file browser (at the given path,\n"
	        "or ~/Music). The first run prompts you to choose a playback device and\n"
	        "remembers it. Most options below are for tinkerers; the browser alone is\n"
	        "all you need for everyday use.\n\n"
	        "Options:\n"
	        "-h                 Print this help message and list devices\n"
	        "-n                 Direct playback (skip the file browser)\n"
	        "-L                 List available playback devices and supported rates\n"
	        "-d <device>        ALSA device for this run (e.g. hw:1,0, plughw:1,0)\n"
	        "-S                 Select the playback device interactively and save it\n"
	        "-f                 Use 32-bit floating-point output\n"
	        "-r                 Recurse into subdirectories\n"
	        "-x                 Shuffle playback order\n"
	        "-s <regexp>        Only play files whose path matches this regex\n"
	        "-t <ext>           Only play files of this type (e.g. flac, mp3, dsf)\n"
	        "-p                 Linux platform optimizations (needs root)\n"
	        "-l                 Loop the directory playlist\n"
	        "-v                 Verbose mode\n"
	        "-P                 Force DSD->PCM conversion (default: native DSD if supported)\n"
	        "-c                 Enable crosstalk cancellation\n"
	        "-D <metres>        Speaker distance for crosstalk (default 0.5)\n"
	        "-T                 Test mode (sine wave: left, right, pan)\n"
	        "\n",
	        argv[0]);
}

int main(int argc, char *argv[])
{
	int flag = 0;
	char *dir = ".";
	char *type = 0;
	char *regexp = 0;
	struct parg_state ps;
	int c;
	int clock = 0;
	int legacy_mode = 0;	// -n: skip the browser, play directly (old behavior)
	int dir_set = 0;	// whether a path was given on the CLI
	int dev_set = 0;	// whether -d was given on the CLI
	int force_select = 0;	// -S: re-run the device picker and save

	// Capture the terminal once and ensure it's always restored on exit.
	if (tcgetattr(0, &g_term_orig) == 0) g_term_saved = 1;
	atexit(term_cleanup);
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = term_on_signal;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);

		struct sigaction sc;
		memset(&sc, 0, sizeof sc);
		sc.sa_handler = crash_handler;
		sigaction(SIGSEGV, &sc, NULL);
		sigaction(SIGABRT, &sc, NULL);
		sigaction(SIGBUS, &sc, NULL);
	}

	theme_load();	// apply ~/.config/aplay+/theme if present

	parg_init(&ps);
	while ((c = parg_getopt(&ps, argc, argv, "hd:frxs:t:pclvD:TLPnS")) != -1) {
		switch (c) {
		case 1: {
			// Expand leading ~ to $HOME
			const char *arg = ps.optarg;
			if (arg[0] == '~' && (arg[1] == '/' || arg[1] == '\0')) {
				const char *home = getenv("HOME");
				if (home) {
					static char expanded[4096];
					snprintf(expanded, sizeof(expanded), "%s%s", home, arg + 1);
					dir = expanded;
				} else {
					dir = (char*)arg;
				}
			} else {
				dir = (char*)arg;
			}
			dir_set = 1;
			break;
		}
		case 'n':
			legacy_mode = 1;
			break;
		case 'L':
			list_alsa_devices();
			return 0;
		case 'd':
			dev = (char*)ps.optarg;
			dev_set = 1;
			break;
		case 'S':
			force_select = 1;
			break;
		case 'f':
			flag |= USE_FLOAT32;
			break;
		case 'r':
			flag |= LS_RECURSIVE;
			break;
		case 'x':
			flag |= LS_RANDOM;
			break;
		case 's':
			regexp = (char*)ps.optarg;
			printf("Search with '%s'.\n", regexp);
			break;
		case 't':
			type = (char*)ps.optarg;
			break;
		case 'p': {
			if (geteuid() != 0) {
				fprintf(stderr, "warning: -p optimizations require root (or CAP_SYS_NICE); some settings may be skipped\n");
			}
			FILE *fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "w");
			if (fp) {
				fprintf(fp, "tsc");
				fclose(fp);
			}
			set_realtime_priority();
			set_cpu("performance");
			clock = 1;
		}
		break;
		case 'c':
			flag |= USE_CROSSTALK;
			crosstalk = 1;
			printf("Crosstalk cancellation enabled.\n");
			break;
		case 'T':
			flag |= USE_TEST_MODE;
			printf("Test mode enabled.\n");
			break;
		case 'l':
			loop_mode = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'D':
			speaker_distance_m = ps.optarg ? atof(ps.optarg) : speaker_distance_m;
			break;
		case 'P':
			force_pcm_dsd = 1;
			break;
		case 'h':
			usage(stderr, argc, argv);
			list_alsa_devices();
			return 0;
		}
	}

	// Resolve the playback device. No silent defaults: the user picks a device
	// once (interactively on first run, saved to ~/.config/aplay+/device) and it
	// persists across sessions. -d overrides for this run; -S re-runs the picker.
	char devbuf[64];
	int have_config = config_load_device(devbuf, sizeof(devbuf));
	if (dev_set) {
		// CLI -d wins for this run. If nothing is saved yet, make it the default.
		if (!have_config) config_save_device(dev);
	} else if (force_select) {
		if (!select_alsa_device(devbuf, sizeof(devbuf))) return 1;
		config_save_device(devbuf);
		dev = devbuf;
	} else if (have_config) {
		dev = devbuf;
	} else {
		// First run, no device chosen yet.
		if (!select_alsa_device(devbuf, sizeof(devbuf))) return 1;
		config_save_device(devbuf);
		dev = devbuf;
	}

	if (flag & USE_TEST_MODE) {
		term_playback_mode();	// raw keys for the test-mode controls
		int format = flag & USE_FLOAT32 ? SND_PCM_FORMAT_FLOAT_LE : 0;
		play_test_mode(format, flag);
	} else {
		// Before starting playback, check once whether the device will silently
		// resample. Use 96000Hz as the probe rate since that's the most common
		// hi-res rate and the one most likely to be resampled by PipeWire.
		if (!dev_is_hw_or_plug(dev) && device_will_silently_resample(dev, 96000)) {
			fprintf(stderr,
			        "\nerror: '%s' will silently resample audio to its internal clock rate.\n"
			        "       This produces corrupted (static) output. Options:\n"
			        "\n"
			        "  1. Use a direct hardware device for bit-perfect playback:\n",
			        dev);
			list_alsa_devices();
			fprintf(stderr,
			        "     Example: aplay+ -d hw:1,0 ...\n"
			        "\n"
			        "  2. Configure PipeWire to clock-switch instead of resampling:\n"
			        "       mkdir -p ~/.config/pipewire/pipewire.conf.d\n"
			        "       cat > ~/.config/pipewire/pipewire.conf.d/99-rates.conf << 'EOF'\n"
			        "context.properties = {\n"
			        "    default.clock.rate = 48000\n"
			        "    default.clock.allowed-rates = [ 44100 48000 88200 96000 ]\n"
			        "}\n"
			        "EOF\n"
			        "       systemctl --user restart pipewire pipewire-pulse\n"
			        "\n");
			return 1;
		}
		struct stat st;
		if (legacy_mode) {
			// -n: direct playback, no browser
			term_playback_mode();	// raw keys for p/n/space/q during playback
			char *de = findExt(dir);
			if (!strcasecmp(de, "m3u") || !strcasecmp(de, "m3u8")) {
				play_m3u(dir, flag);
			} else if (stat(dir, &st) == 0 && S_ISREG(st.st_mode)) {
				// Single file passed — play it directly
				int format = flag & USE_FLOAT32 ? SND_PCM_FORMAT_FLOAT_LE : 0;
				printf("\e[1m\e[35m%s\e[0m\n", dir);
				if (!play_file(dir, format, flag)) {
					fprintf(stderr, "error: unsupported file format: %s\n", dir);
					return 1;
				}
			} else {
				play_dir(dir, type, regexp, flag, NULL);
			}
		} else {
			// Browser (default). A CLI path seeds the start dir; a CLI file
			// seeds the parent dir with the cursor on that file. No path: ~/Music.
			char start[PATH_MAX];
			char focusbuf[PATH_MAX];
			const char *focus = 0;

			if (dir_set && stat(dir, &st) == 0 && S_ISREG(st.st_mode)) {
				char tmp[PATH_MAX];
				snprintf(tmp, sizeof tmp, "%s", dir);
				char *slash = strrchr(tmp, '/');
				if (slash) {
					*slash = 0;
					snprintf(start, sizeof start, "%s", tmp[0] ? tmp : "/");
					snprintf(focusbuf, sizeof focusbuf, "%s", slash + 1);
				} else {
					snprintf(start, sizeof start, ".");
					snprintf(focusbuf, sizeof focusbuf, "%s", tmp);
				}
				focus = focusbuf;
			} else if (dir_set) {
				snprintf(start, sizeof start, "%s", dir);
			} else {
				const char *home = getenv("HOME");
				snprintf(start, sizeof start, "%s/Music", home ? home : ".");
			}

			browse(start, focus, type, regexp, flag);
		}
	}

	if (clock) {
		set_cpu("ondemand");
	}

	return 0;
}
