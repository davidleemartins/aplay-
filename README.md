# aplay+

A minimal, bit-perfect command-line audio player for Linux built on ALSA. Supports
lossless and compressed formats using single-header C libraries — no heavyweight
dependencies, no daemon, no GUI.

> A fork of [**aplay+**](https://github.com/yui0/aplay-) by Yuichiro Nakada, extended by **David Lee Martins** (2026):
> a ranger-like pre-playback file browser, natural + case-insensitive sort,
> per-track now-playing metadata, explicit device selection with persistence and
> auto-recovery on unplug, a four-state bit-perfect/conversion indicator, color
> theming, and `.m3u`/`.m3u8` playback. Released under the same MIT license.

---

## What's different in this fork

This fork builds on Yuichiro Nakada's original aplay+ with a focus on everyday
usability while keeping the bit-perfect, zero-dependency core intact:

- **File browser** — a ranger-like, full-screen terminal chooser (no ncurses) is
  now the default; `-n` opts back into direct playback.
- **Natural, case-insensitive sort** — track numbers order 1→2→…→10 instead of
  lexically, regardless of case.
- **Explicit device selection** — pick your DAC once (interactive picker),
  persisted to `~/.config/aplay+/device`; auto-recovery if a USB DAC is unplugged.
  No silent default/pulse guessing.
- **Bit-perfect status indicator** — a four-state line (BIT PERFECT / MODIFIED /
  FORMAT-CONVERTED / RESAMPLED) tells you honestly what the DAC is doing.
- **Now-playing metadata** — title/artist/album for all formats; duration where
  it's cheap to compute.
- **Color theming** — built-in presets (incl. dracula) + config overrides.
- **Playlist playback** — `.m3u` / `.m3u8` (read-only).
- **Stability & cleanup** — fixed navigation crashes and input-lag on skip/quit,
  and removed dead code.

See [LICENSE](LICENSE) for copyright. Original project:
[github.com/yui0/aplay-](https://github.com/yui0/aplay-).

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
| DSD/DSF | `.dsf` | Native DSD passthrough if supported, else DSD→PCM |
| Playlist | `.m3u`, `.m3u8` | Plays the listed tracks in order (read-only) |

> **Not supported:** Opus (`.opus`). Opus is a distinct codec (SILK + CELT) with
> no good single-header decoder, and adding it would require an external library
> (libopus/libopusfile) — at odds with aplay+'s self-contained, dependency-free
> design. Transcode to FLAC if you need to play these files.

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

The build is unstripped (so the built-in crash handler can print a readable
backtrace). For a smaller binary to distribute, run `strip aplay+`.

### Install (optional)

There's no install target — just run `./aplay+` from the build directory, or copy
the binary somewhere on your `PATH`:

```bash
cp aplay+ ~/.local/bin/      # then run `aplay+` from anywhere
```

The only runtime dependency is ALSA (`libasound.so.2`); all decoders are compiled
in. The binary runs on any Linux system that has ALSA.

### Uninstall

aplay+ keeps no system state. To remove it completely:

```bash
rm -f ~/.local/bin/aplay+          # (or wherever you copied it; or just the build dir)
rm -rf ~/.config/aplay+            # saved device + theme
```

`~/.config/aplay+/` holds just two small text files (`device` and `theme`).

---

## Basic Usage

By default, aplay+ opens an interactive **file browser** to pick what to play.
Pass `-n` to skip the browser and play directly (the classic behavior).

```bash
# Open the browser starting in ~/Music
./aplay+

# Open the browser starting at a given directory
./aplay+ /path/to/music/

# Skip the browser — play a directory directly
./aplay+ -n /path/to/music/

# Skip the browser — play a single file directly
./aplay+ -n /path/to/track.flac

# Play with a specific ALSA device
./aplay+ -d hw:1,0 /path/to/music/
```

### How it works (the intended flow)

aplay+ deliberately separates *browsing* from *listening*. You pick music in the
file browser; pressing `Enter` clears the screen and hands off to a focused
player. When the track/album finishes — or you press `q` — you drop back into the
browser. There is **no browsing while playing**: when the music is on, you listen
to it instead of fiddling with the UI. This split is a conscious design choice,
not a limitation.

A typical session:

1. Launch `aplay+` (first run: choose your DAC — it's remembered).
2. Browse to an album and press `→` to enter it.
3. Press `Enter` on a track to play the album from there (or `Enter` on the
   folder for the whole thing, or `x` / `X` to shuffle).
4. Use `n` / `p` / `space` while it plays; `q` returns you to the browser.
5. `q` again (with confirmation) exits.

**ALSA exclusive output:** aplay+ opens your `hw:` / `plughw:` device directly for
bit-perfect output. While it is playing, that device is held **exclusively** —
other apps' audio (notifications, browser, etc.) will not mix in or play through
it until aplay+ releases the device. That's the deliberate cost of bypassing the
system mixer: nothing is resampled or mixed behind your back. (It also means the
device can show as "busy" if PipeWire/PulseAudio is currently holding it.)

### File Browser

A full-screen, ranger-like chooser. Only directories and playable audio files
are shown.

| Key | Action |
| --- | --- |
| `↑` / `↓` (or `k` / `j`) | Move the cursor |
| `→` / `l` | Open the highlighted directory |
| `←` / `h` / `Backspace` | Go to the parent directory |
| `Enter` | Play (see below) |
| `x` | Shuffle-play the current directory |
| `X` | Shuffle-play the current directory **and subdirectories** |
| `t` | Choose a color theme |
| `d` | Change the playback device |
| `q` / `Esc` | Quit (asks for confirmation) |

`Enter` on a **directory** plays the whole directory. `Enter` on a **track**
plays its directory as an album *starting from that track*, so the in-playback
`n`/`p` keys move to the next/previous track. `Enter` on a **`.m3u` playlist**
plays its entries in order (see [Playlists](#playlists)). Either way the screen clears and
hands off to the normal playlist player (honoring `-r`, `-x`, `-s`, `-t`, `-l`);
when playback ends you return to the browser. A path
on the command line **seeds** the browser's starting location — it does not
bypass it. With no path, the browser starts in `~/Music`.

### Colors / Theming

The quickest way is the in-browser picker: press **`t`** to choose a theme; it
applies immediately and is saved.

For finer control, edit `~/.config/aplay+/theme`. Pick a built-in preset and
optionally override individual elements. Each value is an ANSI SGR code (e.g.
`1;34` = bold blue, or truecolor `38;2;R;G;B`); an empty value means "no color".

```ini
# ~/.config/aplay+/theme
theme=default        # default | mono | contrast | dracula

# optional per-element overrides:
header=1;36          # browser path bar
dir=1;34             # directory entries
file=                # file entries (empty = terminal default)
sel=7                # highlighted row (reverse video)
footer=7             # browser key-hint bar
accent=1;32          # "● BIT PERFECT" indicator
playing=1;35         # now-playing filename
hint=2               # in-player key hint line
warn=1;33            # "◆ MODIFIED" / "◆ FORMAT-CONVERTED"
alert=38;5;214       # "▲ RESAMPLED" (amber)
```

`mono` removes all color (reverse-video selection only); `contrast` uses bright
bold colors; `dracula` is a truecolor palette (needs a 24-bit-capable terminal).

---

## Now-Playing Screen

Each track shows what's playing and, for FLAC, its embedded metadata:

```
  Balcony
  Jenny Lewis — Joy'All
  FLAC · 96000 Hz · 24-bit · 2ch · 2:44 · 2978 kbps
● BIT PERFECT (with S32 32bit)
```

- **Tags** (title / artist / album) are read from **FLAC** & **Ogg Vorbis**
  (Vorbis comments), **MP3** & **DSF** (ID3v2), **M4A/AAC** (MP4 atoms), and
  **WMA** (ASF). WAV shows the technical line only (it rarely carries tags).
- **Duration** and average **bitrate** are shown where the decoder exposes a
  cheap frame count (FLAC, WAV, Ogg). **MP3 deliberately omits duration**:
  `dr_mp3` can only get an accurate length by decoding the whole file, which is
  too expensive to do up front just for a display value. AAC/WMA/DSD likewise
  show tags + codec/rate but no duration.

### Playlists

`.m3u` / `.m3u8` playlists appear in the browser and play in order on `Enter`
(or `aplay+ -n playlist.m3u` from the shell). `n`/`p`/`q` work as usual.

- One file path per line; `#` lines (comments, `#EXTINF`) are ignored.
- Relative paths are resolved against the playlist's own folder, and
  Windows-style `\` separators are accepted — so playlists copied from other
  systems generally just work.
- Missing entries are skipped (shown as `(missing)`) rather than aborting.

Playlist support is read-only for now (no saving/editing).

### Playback status indicator

The bottom line is one honest, color-coded indicator of how faithfully the audio
reaches your DAC, with the negotiated output format appended (e.g.
`● BIT PERFECT (with S32 32bit)`). Color carries the severity:

| Indicator (color) | What happens to the samples | Audiophile verdict |
|---|---|---|
| `● BIT PERFECT` (green) | `hw:` device at the source's native rate; sample values sent untouched (24-bit in an S32 container is still bit-identical) | Pristine |
| `◆ MODIFIED · volume/crosstalk` (yellow) | Native-rate `hw:` path, but `-V` software volume and/or `-c` crosstalk altered the samples | Intentional DSP — not bit-perfect |
| `◆ FORMAT-CONVERTED` (yellow) | `plughw` converted only the sample *format* (rate unchanged) | Benign |
| `▲ RESAMPLED · 96 → 48 kHz` (amber) | The device couldn't do the source rate, so ALSA's plug layer resampled it | Audible change — the one to avoid |

To stay in the green, pick a device that natively supports your files' rates
(`aplay+ -L` shows each device's supported rates), and leave `-V`/`-c` off.

---

## Choosing a Playback Device

aplay+ never guesses your DAC. **The first time you run it**, it lists the
hardware playback devices and asks you to choose one:

```
Select a playback device:

   1) hw:0,3         HDMI 0                       44 48kHz
   2) hw:1,0         ALC285 Analog                48kHz
   3) hw:2,0         Topping E30                  44 48 88 96 176 192kHz

Enter number [1-3], Enter for 1, Esc or q to cancel:
```

The currently-saved device (if any) is marked `← current`. Your choice is saved
to `~/.config/aplay+/device` and **reused automatically on every later run** — no
prompt, no flags needed. To change it later, run `aplay+ -S`, or press `d` in the
browser.

If your saved device is **gone when you try to play** (e.g. you unplugged a USB
DAC), the browser drops straight to this picker so you can choose another, then
continues playing — no cryptic errors to hunt for.

| Flag | Effect on the device |
| --- | --- |
| *(none, first run)* | Prompt, then save as the persistent default |
| `-d hw:X,Y` | Use this device for this run only (does not overwrite a saved default; if nothing is saved yet, it becomes the default) |
| `-S` | Re-run the picker and save a new default |
| `-L` | Just print the device list and exit |

### Bit-Perfect Playback &amp; the plughw Fallback

Use `hw:X,Y` devices for bit-perfect output. The `hw:` prefix bypasses all
software mixing and resampling — the audio goes directly to the hardware at
the source file's native sample rate and bit depth.

aplay+ always *tries* your chosen device bit-perfect first. If the device can't
play a file at its native rate/format, aplay+ automatically retries through the
**plughw plugin for the very same device** (`hw:X,Y` → `plughw:X,Y`) so ALSA
converts it — and the [playback status indicator](#playback-status-indicator)
shows exactly what happened (e.g. `▲ RESAMPLED · 44.1 → 48 kHz (with S16 16bit)`),
so conversion is never silent.

It never guesses a *different* card and never falls back to the PulseAudio/
PipeWire `default` plugin. If you want to force conversion outright, pass a
`plughw:` device directly, e.g. `-d plughw:1,0`.

A device that is **missing or busy** is reported clearly instead of failing
obscurely (and no pointless conversion retry is attempted):

```
error: device 'hw:2,0' is busy (in use by another program).
error: device 'hw:9,9' not found — unplugged? Run 'aplay+ -S' to choose another.
```

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

## DSD Playback

aplay+ plays `.dsf` (DSD) files two ways, chosen automatically:

**Native DSD passthrough** is used when the device supports the
`DSD_U32_BE` format (most native-DSD USB DACs do). The 1-bit DSD stream is sent
straight to the DAC with no filtering, decimation, or conversion — true
bit-perfect DSD. You'll see a line like:

```
5644800Hz DSD 2ch (native DSD_U32_BE @ 176400Hz)
● BIT PERFECT (native DSD)
```

and the DAC's DSD indicator should light up. Native passthrough is only
attempted on `hw:`/`plughw:` devices, since PipeWire/PulseAudio do not expose
native DSD formats.

**DSD→PCM conversion** is the fallback when the DAC can't do native DSD. The
DSD stream is decoded to PCM with an 8-stage low-pass filter to suppress DSD's
ultrasonic noise. DSD64 converts to 88.2kHz, DSD128 to 176.4kHz, DSD256 to
352.8kHz. You'll see:

```
176400Hz 2ch (DSD->PCM)
● BIT PERFECT (with S32 32bit)
```

(or `▲ RESAMPLED …` if the DAC can't take the converted PCM rate). Use `-P` to
force PCM conversion even when native DSD is available — useful for
A/B comparison or if a DAC misbehaves with native DSD.

The native DSD rate sent to ALSA is the DSD bit rate divided by 32 (since
`DSD_U32_BE` packs 32 1-bit samples per 4-byte frame): DSD64 → 88200,
DSD128 → 176400, DSD256 → 352800.

---

## All Options

```
Usage: aplay+ [options] [file-or-dir]
       (no options → interactive browser at the path, or ~/Music)

Options:
  -h                  Print help and list available devices
  -n                  Direct playback (skip the file browser)
  -L                  List available playback devices and supported rates
  -d <device>         ALSA device to use for this run (e.g. hw:1,0)
  -S                  Select the playback device interactively and save it
  -f                  Use 32-bit floating-point output (S32/FLOAT_LE)
  -r                  Recursively search subdirectories
  -x                  Shuffle playback order
  -l                  Loop the playlist continuously
  -s <regexp>         Only play files whose path matches this regex
  -t <ext>            Only play files of this type (e.g. flac, mp3, wav)
  -v                  Verbose output (decoder frame info, etc.)
  -V <volume>         Software volume, 0.0–1.0 (default: 1.0)
  -P                  Force DSD→PCM conversion (default: native DSD if supported)
  -c                  Enable crosstalk cancellation (XTC)
  -D <metres>         Speaker distance for XTC delay calculation (default: 0.5)
  -p                  Linux platform optimisations (requires root)
  -T                  Test mode: plays sine wave through left, right, then pan
```

---

## Option Details

> **You usually don't need any of these.** The default file browser is the
> friendly, everyday way to use aplay+ — launch it, pick your device once, and
> play. The options below are for tinkerers and power users (scripting, A/B
> testing, DSP, low-latency tuning, forcing a specific device or format).

### `-n` — Direct Playback (no browser)

Skips the interactive file browser and plays the given file or directory
immediately — the classic aplay+ behavior. Without `-n`, aplay+ opens the
[file browser](#file-browser) (starting at the given path, or `~/Music` if
none is given).

### `-d <device>` / `-S` — ALSA Device

Specifies the ALSA PCM device for the current run. Common values:

| Device | Behaviour |
|--------|-----------|
| `hw:1,0` | Direct hardware access, bit-perfect (recommended) |
| `plughw:1,0` | Hardware with ALSA format/rate conversion |
| `default` | PipeWire/PulseAudio default sink (resamples unless configured) |

`-d` does not change your saved default unless none is saved yet. To set the
persistent default interactively, run `aplay+ -S`. See
[Choosing a Playback Device](#choosing-a-playback-device) for the full
selection/persistence behavior. Use `aplay+ -L` to find the right `hw:X,Y`
string for your system.

### `-f` — 32-bit Float Output

Requests `FLOAT_LE` (32-bit float) output format from ALSA. If the device does
not support float, choose a device that does, or use a `plughw:` device for
conversion.

Note that `-f` is usually unnecessary: aplay+ already picks a sensible format
per source. 16-bit files play as `S16_LE`, and 24/32-bit files automatically use
`S32_LE` — which is required for many USB DACs that only expose 24-bit endpoints
at high sample rates (sending S16_LE to those produces silence).

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

### `-P` — Force DSD→PCM Conversion

By default, `.dsf` files play via native DSD passthrough when the DAC supports
it (see [DSD Playback](#dsd-playback)). `-P` forces the DSD→PCM conversion path
instead, even on a native-DSD-capable DAC.

```bash
./aplay+ -P -d hw:3,0 /Music/album.dsf
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

A short reminder — `[ p prev  n next  space pause  q quit ]` — is printed above
each track as it starts. The full set of keys that work during playback:

| Key | Action |
|-----|--------|
| `q` or `Esc` | Stop and exit |
| `Space` | Pause / resume |
| `n` | Skip to next track |
| `p` or `b` | Go back to previous track |
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

# Play DSD natively (DAC's DSD light should come on)
./aplay+ -d hw:3,0 "/Music/Album (DSD128)/01. Track.dsf"

# Force DSD->PCM conversion instead of native
./aplay+ -P -d hw:3,0 "/Music/Album (DSD128)/01. Track.dsf"
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

MIT — see [LICENSE](LICENSE).

- Copyright (c) 2023 Yuichiro Nakada — original aplay+
- Copyright (c) 2026 David Lee Martins — this fork

The bundled single-header libraries listed under [Dependencies](#dependencies)
retain their own respective licenses.
