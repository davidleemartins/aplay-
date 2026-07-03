/* Crosstalk-cancellation DSP for aplay+
 *	©2026 David Lee Martins (extracted from aplay+.c, originally ©2017-2025 Yuichiro Nakada)
 *
 * Stereo-only crosstalk cancellation: each channel gets a delayed, attenuated
 * copy of the OPPOSITE channel subtracted, approximating the acoustic
 * crosstalk path between two speakers. The delay is derived from the speaker
 * distance at 343 m/s.
 *
 * apply_crosstalk_cancellation() must be given the format ALSA actually
 * negotiated (AUDIO.actual_format) — S16_LE, S32_LE and FLOAT_LE are
 * supported; 0 is treated as S16_LE (matching AUDIO_init's default).
 * Include after <alsa/asoundlib.h> (for the SND_PCM_FORMAT_* constants).
 * */

#include <stdlib.h>
#include <math.h>

typedef struct {
	int delay_samples;     // Delay in samples (e.g., 3 for 71µs at 44.1kHz)
	float attenuation;     // Attenuation factor (e.g., 0.4)
	float *delay_buffer;   // Ring buffer of past input samples (interleaved L/R)
	int delay_buffer_size; // Size of delay buffer (in floats)
	int delay_index;       // Current index in delay buffer
	float *temp;           // Scratch for integer-format conversion
	int temp_samples;      // Capacity of temp (in floats)
} CrosstalkCancel;

static void init_crosstalk_cancellation(CrosstalkCancel *xtc, int sample_rate,
                                        int channels, float speaker_distance_m)
{
	// Always clear the pointers first: callers keep `xtc` on the stack
	// (uninitialized), and free_crosstalk_cancellation() will free() them.
	xtc->delay_buffer = NULL;
	xtc->temp = NULL;
	xtc->temp_samples = 0;
	if (channels != 2) {
		return;	// stereo only — apply() becomes a no-op
	}
	// 音速343m/sを基準に、スピーカー間距離から遅延を計算
	xtc->delay_samples = (int)(sample_rate * (speaker_distance_m / 343.0));
	xtc->attenuation = 0.4f;
	xtc->delay_buffer_size = xtc->delay_samples * 2;
	if (xtc->delay_buffer_size < 2) {
		xtc->delay_buffer_size = 2;
	}
	xtc->delay_buffer = (float*)calloc(xtc->delay_buffer_size, sizeof(float));
	xtc->delay_index = 0;
}

static void free_crosstalk_cancellation(CrosstalkCancel *xtc)
{
	free(xtc->delay_buffer);
	xtc->delay_buffer = NULL;
	free(xtc->temp);
	xtc->temp = NULL;
	xtc->temp_samples = 0;
}

// Core: process interleaved stereo float samples in place, clamped to [-1,1].
static void xtc_process(CrosstalkCancel *xtc, float *lr, int frames)
{
	for (int i = 0; i < frames; i++) {
		int idx = i * 2;
		int delay_idx = (xtc->delay_index - xtc->delay_samples * 2
		                 + xtc->delay_buffer_size) % xtc->delay_buffer_size;

		// Read the delayed sample BEFORE storing the current one: the ring is
		// exactly delay_samples frames long, so delay_idx == delay_index and
		// the slot still holds the sample from delay_samples frames ago.
		// (The original code stored first and read its own write back — the
		// "delayed" signal was the CURRENT sample, i.e. zero actual delay.)
		float delayed_l = xtc->delay_buffer[delay_idx];
		float delayed_r = xtc->delay_buffer[delay_idx + 1];

		float l = lr[idx], r = lr[idx + 1];
		xtc->delay_buffer[xtc->delay_index] = l;
		xtc->delay_buffer[xtc->delay_index + 1] = r;
		xtc->delay_index = (xtc->delay_index + 2) % xtc->delay_buffer_size;

		lr[idx]     = fmaxf(fminf(l - xtc->attenuation * delayed_r, 1.0f), -1.0f);
		lr[idx + 1] = fmaxf(fminf(r - xtc->attenuation * delayed_l, 1.0f), -1.0f);
	}
}

// Ensure the integer-conversion scratch holds `samples` floats.
static int xtc_temp(CrosstalkCancel *xtc, int samples)
{
	if (samples <= xtc->temp_samples) return 1;
	float *t = (float*)realloc(xtc->temp, samples * sizeof(float));
	if (!t) return 0;
	xtc->temp = t;
	xtc->temp_samples = samples;
	return 1;
}

// `format` must be the format ALSA negotiated (AUDIO.actual_format).
static void apply_crosstalk_cancellation(CrosstalkCancel *xtc, void *buffer,
                                         int frames, int channels, int format)
{
	if (channels != 2 || !xtc->delay_buffer || frames <= 0) {
		return;	// stereo only (or init skipped)
	}

	if (format == SND_PCM_FORMAT_FLOAT_LE) {
		xtc_process(xtc, (float*)buffer, frames);
	} else if (format == SND_PCM_FORMAT_S32_LE) {
		int32_t *data = (int32_t*)buffer;
		int samples = frames * 2;
		if (!xtc_temp(xtc, samples)) return;
		for (int i = 0; i < samples; i++) {
			xtc->temp[i] = data[i] / 2147483648.0f;
		}
		xtc_process(xtc, xtc->temp, frames);
		for (int i = 0; i < samples; i++) {
			float v = xtc->temp[i] * 2147483647.0f;
			data[i] = (int32_t)fmaxf(fminf(v, 2147483647.0f), -2147483648.0f);
		}
	} else {	// SND_PCM_FORMAT_S16_LE (0 = ALSA default S16)
		int16_t *data = (int16_t*)buffer;
		int samples = frames * 2;
		if (!xtc_temp(xtc, samples)) return;
		for (int i = 0; i < samples; i++) {
			xtc->temp[i] = data[i] / 32768.0f;
		}
		xtc_process(xtc, xtc->temp, frames);
		for (int i = 0; i < samples; i++) {
			float v = xtc->temp[i] * 32767.0f;
			data[i] = (int16_t)fmaxf(fminf(v, 32767.0f), -32768.0f);
		}
	}
}
