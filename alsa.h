/* public domain Simple, Minimalistic, Audio library for ALSA
 *	©2017,2020 Yuichiro Nakada
 *
 * Basic usage:
 *	AUDIO a;
 *	AUDIO_init(&a, "plughw:PCH,0", 48000, 2, 32, 1, 0); // device, 48000 samplerate, 2 channels, 32 frame
 *	...
 *	int f = AUDIO_frame(&a); // audio data in a.buffer
 *	...
 *	AUDIO_close(&a);
 *
 * */

// Use the newer ALSA API
#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h>

// Native DSD format — may not be defined in older ALSA headers.
#ifndef SND_PCM_FORMAT_DSD_U32_BE
#define SND_PCM_FORMAT_DSD_U32_BE ((snd_pcm_format_t)52)
#endif

typedef struct {
	snd_pcm_t *handle;
	snd_pcm_uframes_t frames;
	char *buffer;
	int size;
	unsigned int actual_rate;   // rate negotiated by ALSA (may differ from requested)
	int actual_format;          // format negotiated by AUDIO_init_auto (may differ from requested)
} AUDIO;

static int AUDIO_init(AUDIO *thiz, char *dev, unsigned int freq, int ch, int frames, int flag, int format)
{
	// Open PCM device.
	int rc = snd_pcm_open(&thiz->handle, dev, flag ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE, 0);
	if (rc < 0) {
		fprintf(stderr, "unable to open pcm device '%s' (%s)\n", dev, snd_strerror(rc));
		return 1;
	}

	// Allocate a hardware parameters object.
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);

	// Fill it in with default values.
	snd_pcm_hw_params_any(thiz->handle, params);

	// Set the desired hardware parameters.
	// Interleaved mode
	rc = snd_pcm_hw_params_set_access(thiz->handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (rc < 0) {
		fprintf(stderr, "cannot set access type (%s)\n", snd_strerror(rc));
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	// Signed 16-bit little-endian format
	rc = snd_pcm_hw_params_set_format(thiz->handle, params, format ? format : SND_PCM_FORMAT_S16_LE);
	if (rc < 0) {
		fprintf(stderr, "cannot set sample format (%s)\n", snd_strerror(rc));
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	// Channels
	rc = snd_pcm_hw_params_set_channels(thiz->handle, params, ch);
	if (rc < 0) {
		fprintf(stderr, "cannot set channel count (%s)\n", snd_strerror(rc));
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	// Set sample rate (ALSA may negotiate a nearby rate).
	int dir;
	rc = snd_pcm_hw_params_set_rate_near(thiz->handle, params, &freq, &dir);
	if (rc < 0) {
		fprintf(stderr, "cannot set sample rate (%s)\n", snd_strerror(rc));
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}
	thiz->actual_rate = freq; // store what ALSA actually agreed to

	// Set period size (negotiated to nearest supported value).
	thiz->frames = frames;
	rc = snd_pcm_hw_params_set_period_size_near(thiz->handle, params, &thiz->frames, &dir);
	if (rc < 0) {
		fprintf(stderr, "cannot set period size (%s)\n", snd_strerror(rc));
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	// Write the parameters to the driver
	rc = snd_pcm_hw_params(thiz->handle, params);
	if (rc < 0) {
		fprintf(stderr, "unable to set hw parameters (%s)\n", snd_strerror(rc));
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	// Read back actual negotiated period size.
	rc = snd_pcm_hw_params_get_period_size(params, &thiz->frames, &dir);
	if (rc < 0 || thiz->frames == 0) {
		fprintf(stderr, "cannot get period size (%s)\n", rc < 0 ? snd_strerror(rc) : "zero");
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	// 4 bytes/sample covers S32_LE and FLOAT_LE; S16_LE uses half the buffer safely.
	thiz->size = thiz->frames * 4 * ch;
	thiz->buffer = (char*)malloc(thiz->size);
	if (!thiz->buffer) {
		fprintf(stderr, "cannot allocate audio buffer\n");
		snd_pcm_drain(thiz->handle);
		snd_pcm_close(thiz->handle);
		return 1;
	}

	return 0;
}

// Forward declaration — AUDIO_close is defined below.
static void AUDIO_close(AUDIO *thiz);

// Probe whether dev will silently resample by disabling ALSA's resampling plugin
// and checking if a deliberately wrong rate (e.g. 44101Hz) is still "accepted".
// PipeWire/PulseAudio plugins ignore the no-resample flag and accept any rate,
// so if our odd rate is accepted, the device is a middleware sink that will resample.
// Returns 1 if silent resampling is detected, 0 if the device is a real hw sink.
static int device_will_silently_resample(const char *dev, unsigned int freq)
{
	snd_pcm_t *handle;
	if (snd_pcm_open(&handle, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		return 0; // can't open — let AUDIO_init report the error properly
	}

	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(handle, params);

	// Disable ALSA's own resampling plugin. A real hw device will now only
	// accept its native rates. PipeWire/PulseAudio ignores this flag.
	snd_pcm_hw_params_set_rate_resample(handle, params, 0);

	// Try a slightly off rate that no real hardware supports.
	// If it's accepted, the device is doing transparent resampling.
	unsigned int probe_rate = (freq == 44101) ? 44102 : 44101;
	int dir = 0;
	snd_pcm_hw_params_set_rate_near(handle, params, &probe_rate, &dir);

	int resampling = (probe_rate == 44101 || probe_rate == 44102);

	snd_pcm_close(handle);
	return resampling;
}

// Try to open dev as-is; if it fails or negotiates the wrong rate/format, automatically
// retry with plughw: so ALSA's plugin layer handles resampling/format conversion.
// Returns 0 on success, 1 on failure.
static int AUDIO_init_auto(AUDIO *thiz, char *dev, unsigned int freq, int ch, int frames, int flag, int format)
{
	// Allow up to 2Hz of rate fuzz from snd_pcm_hw_params_set_rate_near rounding.
	#define RATE_FUZZ 2

	// Scale period size to maintain at least ~5ms per period at any sample rate.
	// At 32 frames, 192kHz gives only 166µs — smaller than the USB 125µs packet
	// interval — causing silent continuous underruns with no ALSA error reported.
	// Minimum frames = rate * 0.005 (5ms), rounded up to next power of two.
	unsigned int min_frames = freq / 200; // freq * 0.005
	unsigned int scaled_frames = frames;
	while (scaled_frames < min_frames) scaled_frames *= 2;

	if (AUDIO_init(thiz, dev, freq, ch, scaled_frames, flag, format) == 0) {
		unsigned int diff = thiz->actual_rate > freq
		                  ? thiz->actual_rate - freq
		                  : freq - thiz->actual_rate;
		if (diff <= RATE_FUZZ) {
			thiz->actual_format = format ? format : SND_PCM_FORMAT_S16_LE;
			return 0; // close enough — no fallback needed
		}
		// Rate genuinely unsupported — close and fall through to plughw.
		AUDIO_close(thiz);
	}

	// Build plughw: equivalent for the fallback device.
	// "hw:X,Y"   -> "plughw:X,Y"
	// "plughw:*" -> keep as-is (already a plugin device; retry in case format was the issue)
	// anything else (default, pulse, pipewire…) -> "plughw:1,0"
	char fallback[64];
	if (strncmp(dev, "hw:", 3) == 0) {
		snprintf(fallback, sizeof(fallback), "plug%s", dev);
	} else if (strncmp(dev, "plughw:", 7) == 0) {
		snprintf(fallback, sizeof(fallback), "%s", dev);
	} else {
		snprintf(fallback, sizeof(fallback), "plughw:1,0");
	}

	// plughw does not support float; convert to S32_LE which it handles natively.
	int fallback_format = (format == SND_PCM_FORMAT_FLOAT_LE) ? SND_PCM_FORMAT_S32_LE : format;

	// Use a larger period on plughw — at least 1024 frames or the scaled value.
	int fallback_frames = scaled_frames < 1024 ? 1024 : scaled_frames;

	fprintf(stderr, "notice: falling back to %s (format=%s, period=%d) for hw conversion\n",
	        fallback,
	        fallback_format == SND_PCM_FORMAT_S32_LE ? "S32_LE" : "S16_LE",
	        fallback_frames);

	int rc = AUDIO_init(thiz, fallback, freq, ch, fallback_frames, flag, fallback_format);
	if (rc == 0) {
		thiz->actual_format = fallback_format;
	}
	return rc;
}

__attribute__((unused))
static int AUDIO_frame(AUDIO *thiz)
{
	int rc = snd_pcm_readi(thiz->handle, thiz->buffer, thiz->frames);
	if (rc == -EPIPE) {
		// EPIPE means overrun
		fprintf(stderr, "overrun occurred\n");
		snd_pcm_prepare(thiz->handle);
	} else if (rc < 0) {
		fprintf(stderr, "read failed (%s)\n", snd_strerror(rc));
	} else if (rc != (int)thiz->frames) {
		fprintf(stderr, "short read, read %d frames\n", rc);
	}
	return rc;
}

static int AUDIO_play(AUDIO *thiz, char *data, int frames)
{
	int rc = snd_pcm_writei(thiz->handle, data, frames);
	if (rc == -EPIPE) {
		// EPIPE means overrun
		fprintf(stderr, "overrun occurred\n");
		snd_pcm_recover(thiz->handle, rc, 0);
		//snd_pcm_prepare(thiz->handle);
	} else if (rc < 0) {
		fprintf(stderr, "write failed (%s)\n", snd_strerror(rc));
	} else if (rc != frames) {
		fprintf(stderr, "short write, write %d/%d frames\n", rc, (int)thiz->frames);
	}
	return rc;
}

static int AUDIO_play0(AUDIO *thiz)
{
	return AUDIO_play(thiz, thiz->buffer, thiz->frames);
}

static void AUDIO_wait(AUDIO *thiz, int msec)
{
	snd_pcm_wait(thiz->handle, msec);
}

static void AUDIO_close(AUDIO *thiz)
{
	snd_pcm_drain(thiz->handle);
	snd_pcm_close(thiz->handle);
	free(thiz->buffer);
}

// Check whether a device supports a given native DSD format at the given rate.
// Returns 1 if supported, 0 otherwise. Only meaningful for hw: devices —
// PipeWire/plughw will not expose native DSD formats.
static int AUDIO_supports_dsd(char *dev, int dsd_format, unsigned int rate, int ch)
{
	snd_pcm_t *handle;
	if (snd_pcm_open(&handle, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		return 0;
	}
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	if (snd_pcm_hw_params_any(handle, params) < 0) {
		snd_pcm_close(handle);
		return 0;
	}

	int supported = 1;
	if (snd_pcm_hw_params_test_format(handle, params, (snd_pcm_format_t)dsd_format) < 0) supported = 0;
	if (supported && snd_pcm_hw_params_test_rate(handle, params, rate, 0) < 0) supported = 0;
	if (supported && snd_pcm_hw_params_test_channels(handle, params, ch) < 0) supported = 0;

	snd_pcm_close(handle);
	return supported;
}

