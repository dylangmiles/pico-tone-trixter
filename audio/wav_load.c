// audio/wav_load.c — see wav_load.h. RIFF/WAVE → mono float32 via FatFs.
#include "wav_load.h"
#include <string.h>
#include "ff.h"

// WAVE format tags
#define WF_PCM         0x0001
#define WF_FLOAT       0x0003
#define WF_EXTENSIBLE  0xFFFE

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Convert one little-endian sample of `bits`/`fmt` at p to float in [-1,1], matching
// gen_audio_arrays.py: 16→/32768, 24→/8388608, 32int→/2147483648, 32float passthrough,
// 8-bit unsigned→(v-128)/128.
static float sample_to_f(const uint8_t *p, uint16_t bits, uint16_t fmt) {
    if (fmt == WF_FLOAT && bits == 32) {
        uint32_t u = rd32(p);
        float f;
        memcpy(&f, &u, sizeof f);
        return f;
    }
    switch (bits) {
        case 8:  return ((int)p[0] - 128) * (1.0f / 128.0f);          // unsigned PCM
        case 16: return (int16_t)rd16(p) * (1.0f / 32768.0f);
        case 24: {
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v |= (int32_t)0xFF000000;               // sign-extend 24→32
            return v * (1.0f / 8388608.0f);
        }
        case 32: return (int32_t)rd32(p) * (1.0f / 2147483648.0f);
        default: return 0.0f;
    }
}

int wav_load_mono_f32(const char *path, float *dst, int max_samples,
                      uint32_t *rate_out, uint32_t *avail_out) {
    if (rate_out)  *rate_out = 0;
    if (avail_out) *avail_out = 0;

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return WAV_ERR_OPEN;

    UINT br;
    uint8_t hdr[12];
    if (f_read(&f, hdr, 12, &br) != FR_OK || br != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        f_close(&f);
        return WAV_ERR_RIFF;
    }

    // Walk chunks: collect fmt, then stream data.
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    int have_fmt = 0;
    int rc = WAV_ERR_DATA;

    for (;;) {
        uint8_t ch[8];
        if (f_read(&f, ch, 8, &br) != FR_OK || br != 8) break;   // EOF → no data chunk
        uint32_t csize     = rd32(ch + 4);
        FSIZE_t  body      = f_tell(&f);                          // first byte of chunk body
        FSIZE_t  next      = body + csize + (csize & 1);          // start of next chunk (word-aligned)

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fb[40];
            uint32_t take = csize > sizeof(fb) ? sizeof(fb) : csize;
            if (f_read(&f, fb, take, &br) != FR_OK || br != take) { rc = WAV_ERR_FMT; break; }
            fmt      = rd16(fb + 0);
            channels = rd16(fb + 2);
            rate     = rd32(fb + 4);
            bits     = rd16(fb + 14);
            if (fmt == WF_EXTENSIBLE && take >= 26) fmt = rd16(fb + 24);  // real sub-format tag
            have_fmt = (channels > 0 && bits > 0);
            if (f_lseek(&f, next) != FR_OK) { rc = WAV_ERR_READ; break; }
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) { rc = WAV_ERR_FMT; break; }
            if (!((fmt == WF_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                  (fmt == WF_FLOAT && bits == 32))) { rc = WAV_ERR_UNSUP; break; }

            uint16_t bps        = (uint16_t)(bits / 8);
            uint16_t frameBytes = (uint16_t)(bps * channels);
            uint32_t avail      = frameBytes ? (csize / frameBytes) : 0;
            if (avail_out) *avail_out = avail;
            if (rate_out)  *rate_out  = rate;

            // Read whole frames at a time; buffer holds an integer number of frames.
            uint8_t buf[512];
            uint16_t frames_per_buf = (uint16_t)(sizeof(buf) / frameBytes);
            if (frames_per_buf == 0) { rc = WAV_ERR_UNSUP; break; }   // frame > 512B (absurd channel count)

            int out = 0;
            uint32_t remaining = avail;
            while (out < max_samples && remaining > 0) {
                uint32_t want = frames_per_buf;
                if (want > remaining) want = remaining;
                if ((uint32_t)(max_samples - out) < want) want = (uint32_t)(max_samples - out);
                UINT nbytes = (UINT)(want * frameBytes);
                if (f_read(&f, buf, nbytes, &br) != FR_OK || br != nbytes) { rc = WAV_ERR_READ; break; }
                for (uint32_t i = 0; i < want; i++) {
                    const uint8_t *fr = buf + i * frameBytes;
                    float acc = 0.0f;
                    for (uint16_t c = 0; c < channels; c++)
                        acc += sample_to_f(fr + c * bps, bits, fmt);
                    dst[out++] = acc / (float)channels;
                }
                remaining -= want;
            }
            rc = (rc == WAV_ERR_READ) ? WAV_ERR_READ : out;
            break;
        } else {
            if (f_lseek(&f, next) != FR_OK) break;   // unknown chunk — skip to next
        }
    }

    f_close(&f);
    return rc;
}

const char *wav_err_str(int code) {
    switch (code) {
        case WAV_ERR_OPEN:  return "open failed";
        case WAV_ERR_RIFF:  return "not a WAV";
        case WAV_ERR_FMT:   return "bad fmt chunk";
        case WAV_ERR_DATA:  return "no data chunk";
        case WAV_ERR_UNSUP: return "unsupported format/bits";
        case WAV_ERR_READ:  return "read error";
        default:            return code >= 0 ? "ok" : "error";
    }
}
