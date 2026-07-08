// audio/wav_load.h — on-device WAV → mono float32 loader (via FatFs).
//
// Reads a RIFF/WAVE file off the SD card and decodes it to mono float in [-1,1],
// mirroring tools/gen_audio_arrays.py exactly (same per-format divisors, same
// channel down-mix by averaging), so an IR loaded from the card is byte-identical
// to the embedded ir_array_*.h generated from the same WAV. No normalisation.
//
// Supports PCM integer (8/16/24/32-bit) and IEEE float (32-bit), any channel
// count, and WAVE_FORMAT_EXTENSIBLE (reads the real sub-format). Blocking — call
// only at glitch-tolerant moments (preset / IR switch), never in the audio path.
#ifndef TT_WAV_LOAD_H
#define TT_WAV_LOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode `path` to mono float32 into dst[0..max_samples). Returns the number of
// mono samples written (0..max_samples), or a negative WAV_ERR_* code. If the file
// holds more frames than max_samples, it is truncated to max_samples (the caller
// can detect this via avail_out). rate_out / avail_out may be NULL.
//   rate_out  — sample rate from the fmt chunk (Hz)
//   avail_out — total mono frames available in the file (pre-truncation)
int wav_load_mono_f32(const char *path, float *dst, int max_samples,
                      uint32_t *rate_out, uint32_t *avail_out);

#define WAV_ERR_OPEN    (-1)   // f_open failed (missing / unreadable)
#define WAV_ERR_RIFF    (-2)   // not a RIFF/WAVE file
#define WAV_ERR_FMT     (-3)   // no fmt chunk / malformed
#define WAV_ERR_DATA    (-4)   // no data chunk
#define WAV_ERR_UNSUP   (-5)   // unsupported format / bit depth
#define WAV_ERR_READ    (-6)   // f_read failed mid-file

// Human-readable name for a WAV_ERR_* code (for UART diagnostics).
const char *wav_err_str(int code);

#ifdef __cplusplus
}
#endif

#endif // TT_WAV_LOAD_H
