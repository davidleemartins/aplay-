#!/bin/bash
# Generate audio fixtures for the aplay+ test suite into $1 (default: ./fixtures).
# Requires ffmpeg; the DSF file is hand-crafted (ffmpeg has no DSF muxer).
set -e
OUT="${1:-fixtures}"
mkdir -p "$OUT"

META=(-metadata title="Test Title" -metadata artist="Test Artist" -metadata album="Test Album")
SRC=(-f lavfi -i "sine=frequency=440:duration=2" -ac 2)

ffmpeg -v error -y "${SRC[@]}" -c:a flac -sample_fmt s16 "${META[@]}" "$OUT/t16.flac"
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=2:sample_rate=96000" -ac 2 \
       -c:a flac -sample_fmt s32 "${META[@]}" "$OUT/t24-96k.flac"
ffmpeg -v error -y "${SRC[@]}" -c:a pcm_s16le "$OUT/t16.wav"
ffmpeg -v error -y "${SRC[@]}" -c:a pcm_s24le "$OUT/t24.wav"
ffmpeg -v error -y "${SRC[@]}" -c:a libmp3lame -q:a 5 -id3v2_version 3 "${META[@]}" "$OUT/t.mp3"
ffmpeg -v error -y "${SRC[@]}" -c:a libvorbis "${META[@]}" "$OUT/t.ogg"
ffmpeg -v error -y "${SRC[@]}" -c:a aac -movflags +faststart "${META[@]}" "$OUT/t.m4a"
ffmpeg -v error -y "${SRC[@]}" -c:a wmav2 "${META[@]}" "$OUT/t.wma"

# Mono file (crosstalk must no-op) and an empty file (size-0 guard)
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=1" -ac 1 -c:a flac "$OUT/mono.flac"
: > "$OUT/empty.flac"

# ---- hand-crafted DSF: DSD64 stereo, silence pattern, ID3v2 tag at EOF ----
python3 - "$OUT/t.dsf" << 'EOF'
import struct, sys

path = sys.argv[1]
ch, rate, block = 2, 2822400, 4096
blocks = 8                                  # ~0.09s per channel
samples = blocks * block * 8                # DSD bits per channel
data_bytes = blocks * ch * block
data_size = 12 + data_bytes

def id3_frame(fid, text):
    payload = b"\x00" + text.encode("latin-1")  # enc 0 = Latin-1
    return fid + struct.pack(">I", len(payload)) + b"\x00\x00" + payload

frames = (id3_frame(b"TIT2", "DSF Title") + id3_frame(b"TPE1", "DSF Artist")
          + id3_frame(b"TALB", "DSF Album"))
sz = len(frames)
ss = bytes([(sz >> 21) & 0x7F, (sz >> 14) & 0x7F, (sz >> 7) & 0x7F, sz & 0x7F])
id3 = b"ID3\x03\x00\x00" + ss + frames

meta_ptr = 28 + 52 + data_size
total = meta_ptr + len(id3)

# fmt chunk (52 bytes): id, size, version, format id, channel type, channels,
# sampling freq, bits per sample (must be 1), sample count, block size, reserved
fmt = b"fmt " + struct.pack("<QIIIIIIQII", 52, 1, 0, 2, ch, rate, 1, samples, block, 0)
assert len(fmt) == 52

with open(path, "wb") as f:
    f.write(b"DSD " + struct.pack("<QQQ", 28, total, meta_ptr))
    f.write(fmt)
    f.write(b"data" + struct.pack("<Q", data_size))
    f.write(b"\x69" * data_bytes)           # 01101001 — DSD idle-ish pattern
    f.write(id3)
EOF

echo "fixtures written to $OUT"
