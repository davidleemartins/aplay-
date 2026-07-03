/* Unit tests for dsp.h (crosstalk cancellation): S16/S32/FLOAT paths,
 * stereo-only guard, init/free safety, chunked-vs-whole equivalence.
 * ©2026 David Lee Martins
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include "../dsp.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define RATE 44100
#define DIST 0.5f

static void test_mono_noop(void)
{
	CrosstalkCancel xtc;
	init_crosstalk_cancellation(&xtc, RATE, 1, DIST);
	CHECK(xtc.delay_buffer == NULL, "mono init must not allocate a delay buffer");

	float buf[64];
	for (int i = 0; i < 64; i++) buf[i] = 0.5f;
	apply_crosstalk_cancellation(&xtc, buf, 32, 1, SND_PCM_FORMAT_FLOAT_LE);
	for (int i = 0; i < 64; i++)
		if (buf[i] != 0.5f) { CHECK(0, "mono buffer modified at %d", i); break; }

	free_crosstalk_cancellation(&xtc);
	free_crosstalk_cancellation(&xtc);	// double free must be safe
	checks++;	// reached without crash
}

static void test_float_impulse(void)
{
	CrosstalkCancel xtc;
	init_crosstalk_cancellation(&xtc, RATE, 2, DIST);
	CHECK(xtc.delay_buffer != NULL, "stereo init allocates delay buffer");
	int d = xtc.delay_samples;
	CHECK(d > 0 && d < RATE / 100, "plausible delay: %d samples", d);

	int frames = d * 3;
	float *buf = calloc(frames * 2, sizeof(float));
	buf[0] = 1.0f;	// impulse on LEFT at frame 0
	apply_crosstalk_cancellation(&xtc, buf, frames, 2, SND_PCM_FORMAT_FLOAT_LE);

	CHECK(fabsf(buf[0] - 1.0f) < 1e-6, "impulse passes through: %f", buf[0]);
	// After `d` frames, the RIGHT channel gets -attenuation * delayed LEFT.
	float want = -xtc.attenuation;
	CHECK(fabsf(buf[d * 2 + 1] - want) < 1e-4,
	      "right@frame%d: want %f got %f", d, want, buf[d * 2 + 1]);
	// No NaNs anywhere.
	for (int i = 0; i < frames * 2; i++)
		if (isnan(buf[i])) { CHECK(0, "NaN at %d", i); break; }

	free(buf);
	free_crosstalk_cancellation(&xtc);
}

static void test_silence_stays_silent(void)
{
	CrosstalkCancel xtc;
	init_crosstalk_cancellation(&xtc, RATE, 2, DIST);

	int16_t s16[256] = {0};
	apply_crosstalk_cancellation(&xtc, s16, 128, 2, SND_PCM_FORMAT_S16_LE);
	for (int i = 0; i < 256; i++)
		if (s16[i] != 0) { CHECK(0, "s16 silence broken at %d: %d", i, s16[i]); break; }

	int32_t s32[256] = {0};
	apply_crosstalk_cancellation(&xtc, s32, 128, 2, SND_PCM_FORMAT_S32_LE);
	for (int i = 0; i < 256; i++)
		if (s32[i] != 0) { CHECK(0, "s32 silence broken at %d: %d", i, s32[i]); break; }

	checks++;	// both loops passed without tripping
	free_crosstalk_cancellation(&xtc);
}

// The S32 path must produce (scaled) the same result as the FLOAT path.
static void test_s32_matches_float(void)
{
	CrosstalkCancel xa, xb;
	init_crosstalk_cancellation(&xa, RATE, 2, DIST);
	init_crosstalk_cancellation(&xb, RATE, 2, DIST);

	int frames = 512;
	float *fbuf = malloc(frames * 2 * sizeof(float));
	int32_t *ibuf = malloc(frames * 2 * sizeof(int32_t));
	for (int i = 0; i < frames * 2; i++) {
		float v = 0.8f * sinf(2.0f * (float)M_PI * 440.0f * (i / 2) / RATE)
		        * ((i & 1) ? 0.5f : 1.0f);	// different L/R content
		fbuf[i] = v;
		ibuf[i] = (int32_t)(v * 2147483647.0f);
	}

	apply_crosstalk_cancellation(&xa, fbuf, frames, 2, SND_PCM_FORMAT_FLOAT_LE);
	apply_crosstalk_cancellation(&xb, ibuf, frames, 2, SND_PCM_FORMAT_S32_LE);

	float maxerr = 0;
	for (int i = 0; i < frames * 2; i++) {
		float got = ibuf[i] / 2147483648.0f;
		float err = fabsf(got - fbuf[i]);
		if (err > maxerr) maxerr = err;
	}
	CHECK(maxerr < 1e-3f, "S32 path diverges from FLOAT path: maxerr=%g", maxerr);

	free(fbuf); free(ibuf);
	free_crosstalk_cancellation(&xa);
	free_crosstalk_cancellation(&xb);
}

// State must carry across calls: many small chunks == one big chunk.
static void test_chunked_equivalence(void)
{
	CrosstalkCancel xa, xb;
	init_crosstalk_cancellation(&xa, RATE, 2, DIST);
	init_crosstalk_cancellation(&xb, RATE, 2, DIST);

	int frames = 1024;
	float *whole = malloc(frames * 2 * sizeof(float));
	float *parts = malloc(frames * 2 * sizeof(float));
	for (int i = 0; i < frames * 2; i++) {
		whole[i] = parts[i] = 0.7f * sinf(0.01f * i) + 0.2f * sinf(0.13f * i);
	}

	apply_crosstalk_cancellation(&xa, whole, frames, 2, SND_PCM_FORMAT_FLOAT_LE);
	for (int off = 0; off < frames; off += 32)
		apply_crosstalk_cancellation(&xb, parts + off * 2, 32, 2, SND_PCM_FORMAT_FLOAT_LE);

	float maxerr = 0;
	for (int i = 0; i < frames * 2; i++) {
		float err = fabsf(whole[i] - parts[i]);
		if (err > maxerr) maxerr = err;
	}
	CHECK(maxerr < 1e-6f, "chunked processing diverges: maxerr=%g", maxerr);

	free(whole); free(parts);
	free_crosstalk_cancellation(&xa);
	free_crosstalk_cancellation(&xb);
}

int main(void)
{
	test_mono_noop();
	test_float_impulse();
	test_silence_stays_silent();
	test_s32_matches_float();
	test_chunked_equivalence();
	printf("test_dsp: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
