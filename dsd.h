/*
#include <stdio.h>
#define DSD_DECODER_IMPLEMENTATION
#include "dsd_decoder.h"

int main() {
    FILE* file = fopen("sample.dsf", "rb");
    if (!file) {
        printf("Failed to open file\n");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    fseek(file, 0, SEEK_SET);

    uint8_t* data = (uint8_t*)malloc(size);
    fread(data, 1, size, file);
    fclose(file);

    // デコーダ初期化（初期サイズは仮）
    DSDDecoder* decoder = dsd_decoder_init(size, DSD_DEFAULT_SAMPLE_RATE, PCM_DEFAULT_SAMPLE_RATE, 2);
    if (!decoder) {
        free(data);
        return -1;
    }

    if (dsd_decoder_load_dsf(decoder, data, size) != 0) {
        printf("Failed to load DSF data\n");
        dsd_decoder_free(decoder);
        free(data);
        return -1;
    }

    // PCMに変換
    if (dsd_decoder_convert_to_pcm(decoder) != 0) {
        printf("Failed to convert to PCM\n");
        dsd_decoder_free(decoder);
        free(data);
        return -1;
    }

    // PCMデータ取得
    size_t pcm_size;
    const int32_t* pcm_data = dsd_decoder_get_pcm_data(decoder, &pcm_size);

    FILE* out = fopen("output.pcm", "wb");
    fwrite(pcm_data, sizeof(int32_t), pcm_size, out);
    fclose(out);

    dsd_decoder_free(decoder);
    free(data);
    return 0;
}
*/

#ifndef DSD_H
#define DSD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ALSA format definitions
#ifndef SND_PCM_FORMAT_S16_LE
#define SND_PCM_FORMAT_S16_LE 2
#endif
#ifndef SND_PCM_FORMAT_FLOAT_LE
#define SND_PCM_FORMAT_FLOAT_LE 14
#endif
#ifndef SND_PCM_FORMAT_S32_LE
#define SND_PCM_FORMAT_S32_LE 10
#endif
// Native DSD formats (Linux kernel snd_pcm_format_t values)
#ifndef SND_PCM_FORMAT_DSD_U32_BE
#define SND_PCM_FORMAT_DSD_U32_BE 52 // 32 1-bit samples packed in 4 bytes, big-endian
#endif

#define DSD_SAMPLES_PER_BYTE 8
#define MAX_CHANNELS 2

#define DSD_FILTER_STAGES 8

// 2次フィルタの状態と係数を定義
typedef struct {
    double x1, x2;
    double y1, y2;
} FilterState2;

typedef struct {
    double a0, a1, a2;
    double b1, b2;
} FilterCoeff2;

typedef struct {
    FILE* file;
    uint64_t dsd_data_offset;
    uint64_t totalPCMFrameCount;
    uint64_t pcm_frames_processed;

    int sample_rate_dsd;
    int sample_rate_pcm;
    int channels;
    uint32_t block_size_bytes;

    uint8_t* block_buffer;
    size_t block_buffer_size;

    size_t current_dsd_bit_index;

    // Independent coefficients per stage — each tuned to a different cutoff
    // for a steeper overall rolloff than sharing one coefficient set.
    FilterState2 filter_state[MAX_CHANNELS][DSD_FILTER_STAGES];
    FilterCoeff2 filter_coeff[DSD_FILTER_STAGES];

    int initial_rms_estimation_done;
    double current_scale_factor;
    long current_file_pos;

} DSDDecoder;

#ifdef DSD_DECODER_IMPLEMENTATION

// Helper functions (no changes)
static uint32_t read_le32(const uint8_t* buf) { return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24); }
static uint64_t read_le64(const uint8_t* buf) { return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) | ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) | ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56); }

static int dsd_load_next_block(DSDDecoder* decoder) {
    if (!decoder || !decoder->file) return 0;
    size_t bytes_read = fread(decoder->block_buffer, 1, decoder->block_buffer_size, decoder->file);
    decoder->current_dsd_bit_index = 0;
    return bytes_read > 0;
}

// Initialize one biquad lowpass stage (bilinear-transform Butterworth).
static void init_one_biquad(FilterCoeff2* coeff, double cutoff_hz, double sample_rate)
{
    double omega = tan(M_PI * cutoff_hz / sample_rate);
    double denom = 1.0 + sqrt(2.0) * omega + omega * omega;
    coeff->a0 = omega * omega / denom;
    coeff->a1 = 2.0 * coeff->a0;
    coeff->a2 = coeff->a0;
    coeff->b1 = 2.0 * (omega * omega - 1.0) / denom;
    coeff->b2 = (1.0 - sqrt(2.0) * omega + omega * omega) / denom;
}

// Initialize DSD_FILTER_STAGES biquad lowpass stages with geometrically-spaced
// cutoff frequencies. Starting well below Nyquist and stepping up gives a much
// steeper combined rolloff than using the same cutoff for all stages, without
// needing a proper FIR design. The lowest stage handles audio-band shaping;
// higher stages suppress progressively higher DSD noise frequencies.
static void init_filter_stages(FilterCoeff2* coeffs, int stages,
                                double audio_cutoff_hz, double sample_rate_dsd,
                                double decimation_factor)
{
    // Space cutoffs geometrically from audio_cutoff to ~1/4 of DSD rate.
    // This gives steep rolloff starting at 20kHz all the way up to ~700kHz,
    // suppressing the bulk of DSD noise-shaping energy before decimation.
    double top = sample_rate_dsd / (decimation_factor * 2.0); // Nyquist of output
    double ratio = pow(top / audio_cutoff_hz, 1.0 / (stages - 1));
    for (int i = 0; i < stages; ++i) {
        double cutoff = audio_cutoff_hz * pow(ratio, i);
        init_one_biquad(&coeffs[i], cutoff, sample_rate_dsd);
    }
}

// 2次フィルタの適用
static double apply_filter2(FilterState2* state, FilterCoeff2* coeff, double input) {
    double output = coeff->a0 * input + coeff->a1 * state->x1 + coeff->a2 * state->x2
                    - coeff->b1 * state->y1 - coeff->b2 * state->y2;
    state->x2 = state->x1;
    state->x1 = input;
    state->y2 = state->y1;
    state->y1 = output;
    return output;
}

DSDDecoder* dsd_decoder_init_file(FILE* file) {
    if (!file) return NULL;
    DSDDecoder* decoder = (DSDDecoder*)calloc(1, sizeof(DSDDecoder));
    if (!decoder) return NULL;
    decoder->file = file;

    // --- Header Parsing ---
    uint8_t header_buf[80];
    if (fread(header_buf, 1, 28, file) != 28 || strncmp((char*)header_buf, "DSD ", 4) != 0) { free(decoder); return NULL; }
    uint64_t fmt_chunk_offset = 28;
    fseek(file, fmt_chunk_offset, SEEK_SET);
    if (fread(header_buf, 1, 52, file) != 52 || strncmp((char*)header_buf, "fmt ", 4) != 0) { free(decoder); return NULL; }

    uint64_t fmt_chunk_size = read_le64(header_buf + 4);
    decoder->channels = read_le32(header_buf + 24);
    decoder->sample_rate_dsd = read_le32(header_buf + 28);
    uint64_t total_dsd_samples = read_le64(header_buf + 36);
    decoder->block_size_bytes = read_le32(header_buf + 44);

    if (read_le32(header_buf + 32) != 1 || (decoder->channels < 1 || decoder->channels > MAX_CHANNELS) || decoder->block_size_bytes == 0) { free(decoder); return NULL; }
    
    // PCM output rate: decimate by 32 from DSD rate.
    // DSD64  (2.8224MHz)  -> 88200Hz  (decimation factor 32)
    // DSD128 (5.6448MHz)  -> 176400Hz (decimation factor 32)
    // DSD256 (11.2896MHz) -> 352800Hz (decimation factor 32)
    if (decoder->sample_rate_dsd == 2822400)       decoder->sample_rate_pcm = 88200;
    else if (decoder->sample_rate_dsd == 5644800)  decoder->sample_rate_pcm = 176400;
    else if (decoder->sample_rate_dsd == 11289600) decoder->sample_rate_pcm = 352800;
    else decoder->sample_rate_pcm = decoder->sample_rate_dsd / 32;

    size_t decimation_factor = decoder->sample_rate_dsd / decoder->sample_rate_pcm;
    decoder->totalPCMFrameCount = total_dsd_samples / decimation_factor;

    decoder->block_buffer_size = decoder->block_size_bytes * decoder->channels;
    decoder->block_buffer = (uint8_t*)malloc(decoder->block_buffer_size);
    if (!decoder->block_buffer) { free(decoder); return NULL; }

    fseek(file, fmt_chunk_offset + fmt_chunk_size, SEEK_SET);
    char chunk_id[12];
    if (fread(chunk_id, 1, 12, file) != 12 || strncmp(chunk_id, "data", 4) != 0) { free(decoder->block_buffer); free(decoder); return NULL; }
    fseek(file, ftell(file) - 12 + 12, SEEK_SET); // dataチャンクの先頭へ

    // Cutoff at 20kHz for stage 0; higher stages cover up to Nyquist of the
    // PCM output rate, suppressing DSD noise-shaping energy across the spectrum.
    init_filter_stages(decoder->filter_coeff, DSD_FILTER_STAGES,
                       20000.0, decoder->sample_rate_dsd, decimation_factor);
    memset(decoder->filter_state, 0, sizeof(decoder->filter_state));
    
    // 初期ブロックをロード
    dsd_load_next_block(decoder);

    // --- RMS推定の初期化 ---
    decoder->initial_rms_estimation_done = 0;
    decoder->current_scale_factor = 1.0; // デフォルトは1.0 (後で計算)
    decoder->current_file_pos = ftell(file); // 現在のファイル位置を保存
    // ----------------------

    return decoder;
}

void dsd_decoder_free(DSDDecoder* decoder) {
    if (decoder) {
        free(decoder->block_buffer);
        free(decoder);
    }
}

// RMS推定
static void dsd_decoder_estimate_rms(DSDDecoder* decoder, int format) {
    if (decoder->initial_rms_estimation_done) return;

    // ファイルポインタを先頭に戻す
    fseek(decoder->file, decoder->current_file_pos, SEEK_SET);
    // フィルタ状態もリセット
    memset(decoder->filter_state, 0, sizeof(decoder->filter_state));
    decoder->current_dsd_bit_index = 0;
    decoder->pcm_frames_processed = 0;
    dsd_load_next_block(decoder); // 最初のブロックを再ロード

    const int ESTIMATION_FRAMES = decoder->sample_rate_pcm * 2; // 2秒分のPCMフレームで推定
    // 推定用のバッファは、float型で確保すると計算が楽
    float* temp_pcm_buffer = (float*)malloc(ESTIMATION_FRAMES * decoder->channels * sizeof(float));
    if (!temp_pcm_buffer) {
        // メモリ確保失敗の場合は、デフォルトのスケーリング係数を使用
        decoder->current_scale_factor = 1.0; // もしくはより適切なデフォルト値
        decoder->initial_rms_estimation_done = 1;
        return;
    }

    size_t decimation_factor = decoder->sample_rate_dsd / decoder->sample_rate_pcm;
    size_t block_size_bits = decoder->block_size_bytes * DSD_SAMPLES_PER_BYTE;

    double sum_squares[MAX_CHANNELS] = {0.0};
    size_t actual_frames_processed = 0;

    for (size_t i = 0; i < ESTIMATION_FRAMES; ++i) {
        for (int ch = 0; ch < decoder->channels; ++ch) {
            const uint8_t* dsd_channel_data = decoder->block_buffer + (ch * decoder->block_size_bytes);
            double accum = 0.0;
            size_t start_bit = decoder->current_dsd_bit_index;

            for (size_t k = 0; k < decimation_factor; ++k) {
                size_t current_bit = start_bit + k;
                if (current_bit >= block_size_bits) {
                    // 推定中にファイルの終わりに来た場合
                    if (!dsd_load_next_block(decoder)) goto end_estimation_loop;
                    dsd_channel_data = decoder->block_buffer + (ch * decoder->block_size_bytes);
                    start_bit = 0;
                    current_bit = k;
                }
                size_t byte_idx = current_bit / DSD_SAMPLES_PER_BYTE;
                // DSF stores 1-bit data LSB-first (Sony DSF spec).
                int bit_pos = current_bit % DSD_SAMPLES_PER_BYTE;
                double dsd_val = ((dsd_channel_data[byte_idx] >> bit_pos) & 1) ? 1.0 : -1.0;

                double temp = dsd_val;
                for (int stage = 0; stage < DSD_FILTER_STAGES; ++stage) {
                    temp = apply_filter2(&decoder->filter_state[ch][stage],
                                        &decoder->filter_coeff[stage], temp);
                }
                accum += temp;
            }
            // ここではまだ最終的なスケーリングは行わない
            temp_pcm_buffer[i * decoder->channels + ch] = (float)(accum / decimation_factor);
            sum_squares[ch] += pow(temp_pcm_buffer[i * decoder->channels + ch], 2);
        }

        decoder->current_dsd_bit_index += decimation_factor;
        if (decoder->current_dsd_bit_index >= block_size_bits) {
            if (!dsd_load_next_block(decoder)) goto end_estimation_loop;
        }
        actual_frames_processed++;
    }

end_estimation_loop:
    {
      // RMS値を計算し、スケーリング係数を決定
      double average_rms_sq = 0.0;
      if (actual_frames_processed > 0) {
          for (int ch = 0; ch < decoder->channels; ++ch) {
              average_rms_sq += sum_squares[ch];
          }
          average_rms_sq /= (actual_frames_processed * decoder->channels);
      }
      double estimated_rms = sqrt(average_rms_sq);

      // Target -3dBFS RMS — leaves headroom without over-attenuating.
      const double TARGET_RMS = 0.7;

      if (estimated_rms > 1e-9) {
          decoder->current_scale_factor = TARGET_RMS / estimated_rms;
          // Only clamp the ceiling to prevent clipping; don't force a minimum boost.
          if (decoder->current_scale_factor > 8.0) {
              decoder->current_scale_factor = 8.0;
          }
      } else {
          decoder->current_scale_factor = 1.0;
      }
    }

    free(temp_pcm_buffer);
    decoder->initial_rms_estimation_done = 1;

    // ファイルポインタを実際の再生開始位置に戻す
    fseek(decoder->file, decoder->current_file_pos, SEEK_SET);
    // フィルタ状態もリセット
    memset(decoder->filter_state, 0, sizeof(decoder->filter_state));
    decoder->current_dsd_bit_index = 0;
    decoder->pcm_frames_processed = 0;
    dsd_load_next_block(decoder); // 最初のブロックを再ロード
}

size_t dsd_decoder_read_pcm_frames(DSDDecoder* decoder, size_t frames_to_read, void* buffer, int format) {
    if (!decoder || !buffer || frames_to_read == 0 || decoder->pcm_frames_processed >= decoder->totalPCMFrameCount) return 0;

    if (!decoder->initial_rms_estimation_done) {
        dsd_decoder_estimate_rms(decoder, format);
    }

    size_t decimation_factor = decoder->sample_rate_dsd / decoder->sample_rate_pcm;
    if (decimation_factor == 0) return 0;

    size_t frames_read = 0;
    int16_t*  buffer_s16 = (int16_t*)buffer;
    int32_t*  buffer_s32 = (int32_t*)buffer;
    float*    buffer_f32 = (float*)buffer;
    size_t block_size_bits = decoder->block_size_bytes * DSD_SAMPLES_PER_BYTE;

    for (size_t i = 0; i < frames_to_read; ++i) {
        // Process each channel independently
        for (int ch = 0; ch < decoder->channels; ++ch) {
            const uint8_t* dsd_channel_data = decoder->block_buffer + (ch * decoder->block_size_bytes);
            size_t start_bit = decoder->current_dsd_bit_index;

            // CORRECT ORDER: run each DSD bit through the lowpass filter at the
            // DSD sample rate, then take the filter output after decimation_factor
            // bits as the PCM sample. This is proper anti-aliasing decimation.
            // Running the filter after bit-averaging (the previous approach) does
            // not prevent aliasing and causes the muffled sound.
            double pcm_val = 0.0;
            for (size_t k = 0; k < decimation_factor; ++k) {
                size_t current_bit = start_bit + k;
                if (current_bit >= block_size_bits) {
                    if (!dsd_load_next_block(decoder)) goto end_loop;
                    dsd_channel_data = decoder->block_buffer + (ch * decoder->block_size_bytes);
                    start_bit = 0;
                    current_bit = k;
                }
                size_t byte_idx = current_bit / DSD_SAMPLES_PER_BYTE;
                // DSF stores 1-bit data LSB-first (Sony DSF spec).
                int bit_pos = current_bit % DSD_SAMPLES_PER_BYTE;
                double dsd_bit = ((dsd_channel_data[byte_idx] >> bit_pos) & 1) ? 1.0 : -1.0;

                // Filter each bit at DSD rate through all stages
                double filtered = dsd_bit;
                for (int stage = 0; stage < DSD_FILTER_STAGES; ++stage) {
                    filtered = apply_filter2(&decoder->filter_state[ch][stage],
                                            &decoder->filter_coeff[stage], filtered);
                }
                // Accumulate — the last filter output is our decimated sample
                pcm_val = filtered;
            }

            // Apply scale after filtering
            pcm_val *= decoder->current_scale_factor;

            // Write to output buffer in the correct format
            if (format == SND_PCM_FORMAT_FLOAT_LE) {
                if (pcm_val >  1.0) pcm_val =  1.0;
                if (pcm_val < -1.0) pcm_val = -1.0;
                buffer_f32[i * decoder->channels + ch] = (float)pcm_val;
            } else if (format == SND_PCM_FORMAT_S32_LE) {
                // Scale to 32-bit range; DAC uses top 24 bits
                double s = pcm_val * 2147483647.0;
                if (s >  2147483647.0) s =  2147483647.0;
                if (s < -2147483648.0) s = -2147483648.0;
                buffer_s32[i * decoder->channels + ch] = (int32_t)s;
            } else {
                // S16_LE fallback
                double s = pcm_val * 32767.0;
                if (s >  32767.0) s =  32767.0;
                if (s < -32768.0) s = -32768.0;
                buffer_s16[i * decoder->channels + ch] = (int16_t)s;
            }
        }

        // Advance DSD bit index by one decimation block (same for all channels)
        decoder->current_dsd_bit_index += decimation_factor;
        if (decoder->current_dsd_bit_index >= block_size_bits) {
            if (!dsd_load_next_block(decoder)) {
                frames_read++;
                decoder->pcm_frames_processed++;
                goto end_loop;
            }
        }

        decoder->pcm_frames_processed++;
        frames_read++;
        if (decoder->pcm_frames_processed >= decoder->totalPCMFrameCount) goto end_loop;
    }

end_loop:
    return frames_read;
}

// Native DSD rate for ALSA: the DSD bit rate divided by 32, because DSD_U32_BE
// packs 32 1-bit samples into each 4-byte frame. DSD64 (2.8224MHz) -> 88200,
// DSD128 (5.6448MHz) -> 176400, DSD256 -> 352800.
static int dsd_native_rate(DSDDecoder* decoder) {
    return decoder->sample_rate_dsd / 32;
}

// Total native DSD frames: each frame carries 32 DSD bits per channel.
static uint64_t dsd_native_total_frames(DSDDecoder* decoder) {
    // totalPCMFrameCount was DSD_samples / decimation_factor; recover DSD sample
    // count and divide by 32 bits-per-native-frame.
    size_t decimation_factor = decoder->sample_rate_dsd / decoder->sample_rate_pcm;
    uint64_t total_dsd_samples = decoder->totalPCMFrameCount * decimation_factor;
    return total_dsd_samples / 32;
}

// Bit-reverse a byte (LSB-first DSF -> MSB-first for DSD_U32_BE).
static inline uint8_t dsd_bitrev8(uint8_t v) {
    return (uint8_t)((v * 0x0202020202ULL & 0x010884422010ULL) % 1023);
}

// Reset decoder to the start of audio data for native playback. The first block
// is already loaded by init, but if RMS estimation or PCM playback ran first,
// this rewinds cleanly. Call before the first dsd_read_native_u32be.
static void dsd_native_reset(DSDDecoder* decoder) {
    // current_file_pos was saved AFTER the first block load in init, so to get
    // back to the true start of data we must seek to (current_file_pos - one block).
    long data_start = decoder->current_file_pos - (long)decoder->block_buffer_size;
    if (data_start < 0) data_start = decoder->current_file_pos;
    fseek(decoder->file, data_start, SEEK_SET);
    decoder->current_dsd_bit_index = 0;
    decoder->pcm_frames_processed = 0;
    dsd_load_next_block(decoder);
}

// Read native DSD frames packed as DSD_U32_BE. Each output frame is, per channel,
// 4 bytes holding 32 consecutive DSD bits. No filtering, no decimation, no scaling
// — the raw 1-bit stream goes straight to the DAC. Returns frames written.
//
// The DAC expects "oldest bits in MSB" (per aplay's DSD_U32_BE description).
// DSF stores bits LSB-first (oldest sample in bit 0), so each byte must be
// bit-reversed. The 4 bytes are then written directly in big-endian order
// (earliest byte to lowest address) — NOT assembled into a native uint32, which
// would be stored little-endian on x86 and scramble the byte order.
size_t dsd_read_native_u32be(DSDDecoder* decoder, size_t frames_to_read, void* buffer) {
    if (!decoder || !buffer || frames_to_read == 0) return 0;

    uint8_t* out = (uint8_t*)buffer;
    size_t frames_written = 0;
    size_t block_size_bits = decoder->block_size_bytes * DSD_SAMPLES_PER_BYTE;

    for (size_t f = 0; f < frames_to_read; ++f) {
        // Check block boundary ONCE per frame, before reading any channel, so all
        // channels stay synchronized to the same bit position within the block.
        if (decoder->current_dsd_bit_index + 32 > block_size_bits) {
            if (!dsd_load_next_block(decoder)) goto done;
            // dsd_load_next_block resets current_dsd_bit_index to 0
        }

        size_t byte_idx = decoder->current_dsd_bit_index / 8;

        for (int ch = 0; ch < decoder->channels; ++ch) {
            const uint8_t* ch_data = decoder->block_buffer + (ch * decoder->block_size_bytes);
            uint8_t* dst = out + (f * decoder->channels + ch) * 4;
            // DSF stores DSD LSB-first (DSD_LSBF_PLANAR); DSD_U32_BE wants MSB-first
            // ("oldest bits in MSB"), so bit-reverse each byte. Bytes are written
            // directly in big-endian order (earliest byte at lowest address) — not
            // assembled into a native uint32, which would store little-endian on x86.
            dst[0] = dsd_bitrev8(ch_data[byte_idx + 0]);
            dst[1] = dsd_bitrev8(ch_data[byte_idx + 1]);
            dst[2] = dsd_bitrev8(ch_data[byte_idx + 2]);
            dst[3] = dsd_bitrev8(ch_data[byte_idx + 3]);
        }

        decoder->current_dsd_bit_index += 32;
        decoder->pcm_frames_processed++;
        frames_written++;
    }

done:
    return frames_written;
}

#endif // DSD_DECODER_IMPLEMENTATION
#endif // DSD_H
