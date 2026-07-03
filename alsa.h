/* public domain Simple, Minimalistic, Audio library for ALSA
 *	©2017,2020 Yuichiro Nakada
 *	Modifications ©2026 David Lee Martins
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
	int can_pause;              // hardware supports snd_pcm_pause (many USB DACs don't)
} AUDIO;

// Device-name classification helpers (used for the bit-perfect indicator, the
// plughw fallback and native-DSD probing).
static int dev_is_hw(const char *dev)   { return strncmp(dev, "hw:", 3) == 0; }
static int dev_is_plug(const char *dev) { return strncmp(dev, "plughw:", 7) == 0; }
static int dev_is_hw_or_plug(const char *dev) { return dev_is_hw(dev) || dev_is_plug(dev); }

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

	// Whether the hardware supports pause — key()'s space handler emulates
	// pause with drop+prepare when it doesn't (common on USB DACs).
	thiz->can_pause = snd_pcm_hw_params_can_pause(params);

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

// Human-readable name for the ALSA output sample format.
static const char *pcm_fmt_name(int f)
{
	switch (f) {
	case SND_PCM_FORMAT_FLOAT_LE: return "FLOAT 32bit";
	case SND_PCM_FORMAT_S32_LE:   return "S32 32bit";
	case SND_PCM_FORMAT_S16_LE:   return "S16 16bit";
	default:                      return "PCM";
	}
}

// Format a rate in kHz: 176400 -> "176.4", 48000 -> "48", 44100 -> "44.1".
static const char *khz_str(char *buf, size_t n, unsigned int rate)
{
	if (rate % 1000 == 0) snprintf(buf, n, "%u", rate / 1000);
	else                  snprintf(buf, n, "%.1f", rate / 1000.0);
	return buf;
}

// Can dev be opened at all right now? (false = unplugged/missing/busy.)
static int device_available(const char *dev)
{
	snd_pcm_t *h;
	if (snd_pcm_open(&h, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return 0;
	snd_pcm_close(h);
	return 1;
}

// Does dev support this exact rate natively? Used to tell a format-only
// conversion (rate supported) from genuine resampling (rate not supported).
static int device_supports_rate(const char *dev, unsigned int rate)
{
	snd_pcm_t *h;
	if (snd_pcm_open(&h, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return 0;
	snd_pcm_hw_params_t *p;
	snd_pcm_hw_params_alloca(&p);
	int ok = 0;
	if (snd_pcm_hw_params_any(h, p) >= 0)
		ok = (snd_pcm_hw_params_test_rate(h, p, rate, 0) == 0);
	snd_pcm_close(h);
	return ok;
}

// Try to open dev as-is; if it fails or negotiates the wrong rate/format, automatically
// retry with plughw: so ALSA's plugin layer handles resampling/format conversion.
// Prints a one-line playback-status indicator (color carries severity):
//   green  ● BIT PERFECT          hw:, native rate, no app DSP
//   yellow ◆ MODIFIED · crosstalk hw: native, but -c crosstalk alters samples
//   yellow ◆ FORMAT-CONVERTED     plughw, rate unchanged (format only)
//   amber  ▲ RESAMPLED · A → B kHz plughw, sample rate changed (audible)
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

	// Probe the device up front so a dead/busy device gives a clear, actionable
	// error instead of a confusing cascade of open failures — and so we don't
	// attempt a plughw fallback that would fail for the very same reason. A clean
	// open/close also leaves the device in a proper state.
	snd_pcm_t *probe;
	int orc = snd_pcm_open(&probe, dev,
	                       flag ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE, 0);
	if (orc < 0) {
		if (orc == -EBUSY) {
			fprintf(stderr, "error: device '%s' is busy (in use by another program).\n", dev);
		} else if (orc == -ENOENT || orc == -ENODEV) {
			fprintf(stderr, "error: device '%s' not found — unplugged? Run 'aplay+ -S' to choose another.\n", dev);
		} else {
			fprintf(stderr, "error: cannot open device '%s': %s\n", dev, snd_strerror(orc));
		}
		return 1;
	}
	snd_pcm_close(probe);	// reachable — release it before the real open

	// Try the device as-is for bit-perfect playback.
	int resampling = 0;	// will plughw change the sample rate (vs format only)?
	unsigned int dev_rate = 0;	// device's nearest rate, when known
	int rc = AUDIO_init(thiz, dev, freq, ch, scaled_frames, flag, format);
	if (rc == 0) {
		unsigned int diff = thiz->actual_rate > freq
		                  ? thiz->actual_rate - freq
		                  : freq - thiz->actual_rate;
		if (diff <= RATE_FUZZ) {
			thiz->actual_format = format ? format : SND_PCM_FORMAT_S16_LE;
			const char *fmt = pcm_fmt_name(thiz->actual_format);
			if (dev_is_hw(dev) && !crosstalk) {
				printf("%s\xe2\x97\x8f BIT PERFECT\e[0m %s(with %s)\e[0m\n",
				       theme.accent, theme.hint, fmt);
			} else if (dev_is_hw(dev)) {
				printf("%s\xe2\x97\x86 MODIFIED \xc2\xb7 crosstalk\e[0m %s(with %s)\e[0m\n",
				       theme.warn, theme.hint, fmt);
			} else {
				// User passed a plughw/other device directly — goes through the
				// plug layer (format conversion possible), so not bit-perfect.
				printf("%s\xe2\x97\x86 FORMAT-CONVERTED\e[0m %s(with %s)\e[0m\n",
				       theme.warn, theme.hint, fmt);
			}
			fflush(stdout);
			return 0; // device plays this stream natively
		}
		// Opened, but can't run at the source's native rate — plughw will resample.
		dev_rate = thiz->actual_rate;
		resampling = 1;
		AUDIO_close(thiz);
	} else {
		// Open/params failed without a rate negotiation. If the device supports
		// the source rate, plughw only converts format; otherwise it resamples.
		resampling = !device_supports_rate(dev, freq);
	}

	// Build a plughw fallback for the SAME device so ALSA's plug layer handles
	// rate/format conversion. We never guess a different card, and never fall
	// back to the pulse/'default' plugin.
	char fallback[64];
	if (dev_is_hw(dev)) {
		snprintf(fallback, sizeof(fallback), "plug%s", dev);	// hw:X,Y -> plughw:X,Y
	} else if (dev_is_plug(dev)) {
		snprintf(fallback, sizeof(fallback), "%s", dev);	// already a plug device
	} else {
		fprintf(stderr,
		        "error: '%s' can't play this stream and is not a hw:/plughw: device,\n"
		        "       so no conversion fallback applies. Choose a hw:X,Y device (aplay+ -L).\n",
		        dev);
		return 1;
	}

	// plughw cannot output float; request S32_LE, which it converts to natively.
	int fallback_format = (format == SND_PCM_FORMAT_FLOAT_LE) ? SND_PCM_FORMAT_S32_LE : format;
	int fallback_frames = scaled_frames < 1024 ? 1024 : scaled_frames;

	rc = AUDIO_init(thiz, fallback, freq, ch, fallback_frames, flag, fallback_format);
	if (rc == 0) {
		// Normalize 0 ("ALSA default") to S16_LE, matching what AUDIO_init uses,
		// so the indicator shows "S16 16bit" rather than a generic "PCM".
		thiz->actual_format = fallback_format ? fallback_format : SND_PCM_FORMAT_S16_LE;
		const char *fmt = pcm_fmt_name(thiz->actual_format);
		char a[16], b[16];
		if (resampling && dev_rate)
			printf("%s\xe2\x96\xb2 RESAMPLED \xc2\xb7 %s \xe2\x86\x92 %s kHz\e[0m %s(with %s)\e[0m\n",
			       theme.alert, khz_str(a, sizeof a, freq), khz_str(b, sizeof b, dev_rate),
			       theme.hint, fmt);
		else if (resampling)
			printf("%s\xe2\x96\xb2 RESAMPLED \xc2\xb7 %s kHz source\e[0m %s(with %s)\e[0m\n",
			       theme.alert, khz_str(a, sizeof a, freq), theme.hint, fmt);
		else
			printf("%s\xe2\x97\x86 FORMAT-CONVERTED\e[0m %s(with %s)\e[0m\n",
			       theme.warn, theme.hint, fmt);
		fflush(stdout);
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
		// EPIPE on playback means underrun. Recover and rewrite the chunk once
		// so the audio it carried isn't dropped.
		fprintf(stderr, "underrun occurred\n");
		snd_pcm_recover(thiz->handle, rc, 0);
		rc = snd_pcm_writei(thiz->handle, data, frames);
	}
	if (rc < 0) {
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
	// On a user interrupt (skip/quit) drop the buffered audio so the keypress is
	// responsive; on natural track end, drain so the tail plays out.
	if (g_interrupt) snd_pcm_drop(thiz->handle);
	else             snd_pcm_drain(thiz->handle);
	snd_pcm_close(thiz->handle);
	free(thiz->buffer);
	g_interrupt = 0;
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

