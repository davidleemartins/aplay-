# aplay+

A minimal, bit-perfect command-line audio player for Linux built on ALSA. Supports
lossless and compressed formats using single-header C libraries — no heavyweight
dependencies, no daemon, no GUI.

---

## Supported Formats

| Format | Extension | Notes |
|--------|-----------|-------|
| FLAC | `.flac` | Lossless; 16/24/32-bit |
| WAV | `.wav` | Uncompressed PCM |
| MP3 | `.mp3` | Via dr_mp3 |
| Ogg Vorbis | `.ogg` | Via stb_vorbis |
| AAC | `.m4a`, `.mp4` | Raw AAC and MP4 container |
| WMA | `.wma` | Windows Media Audio |
| DSD/DSF | `.dsf` | DSD audio in DSF container |

---

## Building

Install the ALSA development library, then run `make`:

```bash
# Debian/Ubuntu
sudo apt install libasound2-dev

# Fedora/RHEL
sudo dnf install alsa-lib-devel

make
```

---

## Basic Usage

```bash
# Play a directory of files
./aplay+ /path/to/music/

# Play a single file
./aplay+ /path/to/track.flac

# Play with a specific ALSA device
./aplay+ -d hw:1,0 /path/to/music/
```

---

## Choosing a Playback Device

Run `aplay+ -L` to list all available hardware devices with their supported sample rates:

```
Available playback devices:

  Device         Name                         Rates
  ------         ----                         -----
  hw:0,3         HDMI 0                       44 48kHz
  hw:1,0         ALC285 Analog                44 48 96 192kHz
```

Use the **Device** string directly with the `-d` flag:

```bash
./aplay+ -d hw:1,0 /path/to/music/
```

### Bit-Perfect Playback

Use `hw:X,Y` devices for bit-perfect output. The `hw:` prefix bypasses all
software mixing and resampling — the audio goes directly to the hardware at
the source file's native sample rate and bit depth.

**Avoid `-d default`** unless PipeWire is configured for clock-switching (see
below). By default, PipeWire locks its internal clock to 48kHz and silently
resamples everything to match — aplay+ detects this and refuses to play rather
than output corrupted audio.

### PipeWire Clock-Switching (optional)

If you want to use `-d default` with hi-res audio, configure PipeWire to
switch its clock rate to match the source:

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cat > ~/.config/pipewire/pipewire.conf.d/99-rates.conf << 'EOF'
context.properties = {
    default.clock.rate = 48000
    default.clock.allowed-rates = [ 44100 48000 88200 96000 ]
}
EOF
systemctl --user restart pipewire pipewire-pulse
```

After this, PipeWire will switch to 96kHz when a 96kHz stream opens and no
resampling occurs.

---

## All Options

```
Usage: aplay+ [options] file-or-dir

Options:
  -h                  Print help and list available devices
  -L                  List available playback devices and supported rates
  -d <device>         ALSA device to use (default: hw:0,0)
  -f                  Use 32-bit floating-point output (S32/FLOAT_LE)
  -r                  Recursively search subdirectories
  -x                  Shuffle playback order
  -l                  Loop the playlist continuously
  -s <regexp>         Only play files whose path matches this regex
  -t <ext>            Only play files of this type (e.g. flac, mp3, wav)
  -v                  Verbose output (decoder frame info, etc.)
  -V <volume>         Software volume, 0.0–1.0 (default: 1.0)
  -c                  Enable crosstalk cancellation (XTC)
  -D <metres>         Speaker distance for XTC delay calculation (default: 0.5)
  -p                  Linux platform optimisations (requires root)
  -T                  Test mode: plays sine wave through left, right, then pan
```

---

## Option Details

### `-d <device>` — ALSA Device

Specifies the ALSA PCM device. Common values:

| Device | Behaviour |
|--------|-----------|
| `hw:1,0` | Direct hardware access, bit-perfect (recommended) |
| `plughw:1,0` | Hardware with ALSA format/rate conversion |
| `default` | PipeWire/PulseAudio default sink (resamples unless configured) |

Use `aplay+ -L` to find the right `hw:X,Y` string for your system.

### `-f` — 32-bit Float Output

Requests `FLOAT_LE` (32-bit float) output format from ALSA instead of the
default `S16_LE`. Useful if your DAC or interface handles float natively.
If the device does not support float, aplay+ automatically falls back to
`S32_LE` via `plughw`.

### `-r` — Recursive

Descend into subdirectories when scanning a directory for audio files.

```bash
./aplay+ -r /path/to/music/
```

### `-x` — Shuffle

Randomise the playback order using a Fisher-Yates shuffle seeded from
`/dev/urandom`.

```bash
./aplay+ -rx /path/to/music/
```

### `-l` — Loop

Repeat the playlist indefinitely until interrupted with `q` or Ctrl-C.

### `-s <regexp>` — Regex Filter

Only play files whose full path matches the regular expression. The match
is case-insensitive.

```bash
# Play only ZARD tracks
./aplay+ -rx -d hw:1,0 /Music/ -s ZARD

# Exclude instrumentals
./aplay+ -rfx -d hw:1,0 /Music/ -s '^(?!.*nstrumental).*$'
```

### `-t <ext>` — File Type Filter

Only play files with this extension (case-insensitive exact match).

```bash
./aplay+ -t flac /path/to/music/
./aplay+ -t mp3 /path/to/music/
```

### `-v` — Verbose

Print decoder frame information during playback.

### `-V <volume>` — Software Volume

Scale output volume in software. Range is `0.0` (silent) to `1.0` (full,
default). Applied before writing to ALSA so there is no interaction with the
hardware mixer.

```bash
./aplay+ -V 0.7 /path/to/music/
```

### `-c` — Crosstalk Cancellation

Enables binaural crosstalk cancellation (XTC). This applies a delay-based
filter to reduce the bleed of the left channel into the right ear and vice
versa, producing a wider stereo image on speakers.

Toggle XTC on/off at any time with the `c` key during playback. Adjust
attenuation with `+` and `-`.

### `-D <metres>` — Speaker Distance

Sets the inter-speaker distance used to calculate the XTC delay, in metres.
Default is `0.5` (50 cm). Only meaningful when `-c` is also used.

```bash
./aplay+ -c -D 0.8 /path/to/music/
```

### `-p` — Platform Optimisations

Applies Linux system-level tuning for lower latency (requires root or
`CAP_SYS_NICE`):

- Sets the CPU frequency governor to `performance`
- Switches the system clocksource to `tsc`
- Elevates the process to real-time FIFO scheduling priority

Restores the CPU governor to `ondemand` when playback ends.

```bash
sudo ./aplay+ -p -d hw:1,0 /path/to/music/
```

### `-T` — Test Mode

Plays a synthesised sine wave cycling through left channel, right channel,
and a left-to-right pan. Useful for verifying speaker wiring and channel
balance. Press `q` to exit.

---

## Keyboard Controls

These keys work during playback:

| Key | Action |
|-----|--------|
| `q` or `Esc` | Stop and exit |
| `Space` | Pause / resume |
| `n` or `→` | Skip to next track |
| `p` or `b` or `←` | Go back to previous track |
| `c` | Toggle crosstalk cancellation |
| `+` | Increase XTC attenuation |
| `-` | Decrease XTC attenuation |

---

## Examples

```bash
# List available devices
./aplay+ -L

# Play a single track
./aplay+ -d hw:1,0 "/Music/Artist/track.flac"

# Play an album in order
./aplay+ -d hw:1,0 "/Music/Jenny Lewis - Joy'All (2023) [24B-96kHz]/"

# Shuffle an entire library recursively
./aplay+ -rxd hw:1,0 /Music/

# Play only FLAC files, shuffled
./aplay+ -rx -t flac -d hw:1,0 /Music/

# Play with crosstalk cancellation at 80cm speaker spacing
./aplay+ -c -D 0.8 -d hw:1,0 /Music/

# Loop an album at reduced volume
./aplay+ -l -V 0.8 -d hw:1,0 "/Music/Artist/Album/"
```

---

## Linux System Tuning (Optional)

For lowest latency and most consistent playback, especially at high sample rates:

### CPU Governor

```bash
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$f"
done
```

### I/O Scheduler (for SSDs)

```bash
# /etc/udev/rules.d/60-ioschedulers.rules
ACTION=="add|change", KERNEL=="sd[a-z]|mmcblk[0-9]*", \
    ATTR{queue/rotational}=="0", ATTR{queue/scheduler}="none"
ACTION=="add|change", KERNEL=="sd[a-z]", \
    ATTR{queue/rotational}=="1", ATTR{queue/scheduler}="bfq"
```

### VM Dirty Pages

```bash
# /etc/sysctl.conf
vm.dirty_ratio = 40
vm.dirty_background_ratio = 10
vm.dirty_expire_centisecs = 3000
vm.dirty_writeback_centisecs = 500
vm.overcommit_memory = 1
```

Apply with `sudo sysctl -p`.

---

## Dependencies

All decoding libraries are vendored as single-header files — no external packages
needed beyond ALSA:

- [dr_flac](https://github.com/mackron/dr_libs) — FLAC decoding
- [dr_wav](https://github.com/mackron/dr_libs) — WAV decoding
- [dr_mp3](https://github.com/mackron/dr_libs) — MP3 decoding
- [stb_vorbis](https://github.com/nothings/stb) — Ogg Vorbis decoding
- [minimp3](https://github.com/lieff/minimp3) — MP3 (alternate)
- [parg](https://github.com/jibsen/parg) — Argument parsing

---

## License

MIT
