// audio/backing.cpp — SD-streamed backing tracks. See backing.h for the contract.
#include "audio/backing.h"
#include "ff.h"
#include "i2s/i2s.h"
#include "pico/time.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

// The engine rate is NOT a local constant -- take it from the I2S header, which is the
// single source of truth. It was hardcoded to 96000 here initially, copied from stale
// comments in main.cpp; the hardware runs at 48 kHz, so every track played at half
// speed. A local copy of someone else's constant is a bug waiting for a rate change.
#define ENGINE_RATE        ((uint32_t)I2S_SAMPLE_RATE)

// Ring holds int16 mono at the FILE's rate; resampling happens on the way out.
// 32768 samples = 64 kB = 0.68 s at 48 kHz. Sized for stall tolerance, which is the
// real risk here: bit-banged SPI reads are bursty and an OLED/menu repaint can steal
// several ms. Must stay a power of two (the index masking assumes it).
#define RING_SAMPLES       32768u
#define RING_MASK          (RING_SAMPLES - 1u)

// Bounded read per service() call. The foreground loop iterates about once per
// 256-sample block = 5.33 ms at 48 kHz, so ~187 iterations/s. At 1 kB each that is
// ~187 kB/s of capacity against the 96 kB/s a 48 kHz 16-bit mono file needs -- under
// 2x margin, tighter than it looks, which is why fill_chunk() bursts 4x while the
// ring is below a quarter. Small on purpose: this read sits directly in the
// block-publication path, so a bigger chunk trades underrun margin for jitter.
// `bk stat` reports the truth (underruns + worst service time); raise this only if
// that says you need to.
#define CHUNK_BYTES        1024u

struct Entry { char name[BACKING_NAME_LEN]; char path[64]; };

static Entry    s_files[BACKING_MAX_FILES];
static int      s_count   = 0;
static int      s_cur     = -1;
static bool     s_playing = false;

static FIL      s_fil;
static int16_t  s_ring[RING_SAMPLES];
static uint32_t s_head = 0, s_tail = 0;     // monotonic; mask to index
static uint64_t s_phase = 0;                // 32.32 fixed point, fractional read pos
static uint64_t s_phase_inc = 0;            // file_rate / ENGINE_RATE in 32.32

static uint16_t s_bits = 16, s_chans = 1;
static uint32_t s_rate = 48000;
static uint32_t s_data_start = 0, s_data_bytes = 0, s_data_pos = 0;
static bool     s_float_fmt = false;

static float    s_level = 0.8f;   // set by ear on hardware 2026-08-23
static uint32_t s_underruns = 0, s_max_service_us = 0;

static uint8_t  s_raw[CHUNK_BYTES];

const char *backing_err_str(int c) {
    switch (c) {
        case BACKING_ERR_NOFILE: return "no such backing file";
        case BACKING_ERR_OPEN:   return "open failed";
        case BACKING_ERR_RIFF:   return "not a RIFF/WAVE file";
        case BACKING_ERR_FMT:    return "no/bad fmt chunk";
        case BACKING_ERR_DATA:   return "no data chunk";
        case BACKING_ERR_UNSUP:  return "unsupported format";
        default:                 return "ok";
    }
}

// ---------------------------------------------------------------- scanning

void backing_scan(void) {
    s_count = 0;
    DIR dir;
    if (f_opendir(&dir, "/tonetrix/backing") != FR_OK) return;
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] && s_count < BACKING_MAX_FILES) {
        if (fno.fattrib & AM_DIR) continue;
        // macOS writes an AppleDouble sidecar per file on FAT volumes ("._blues92.wav").
        // They end in .wav, so without this they enumerate as playable tracks and then
        // fail with "not a RIFF/WAVE file". Cards get loaded on a Mac; filter them here.
        if (fno.fname[0] == '.') continue;
        size_t l = strlen(fno.fname);
        if (l < 5 || strcasecmp(fno.fname + l - 4, ".wav") != 0) continue;
        Entry *e = &s_files[s_count++];
        strncpy(e->name, fno.fname, sizeof e->name - 1); e->name[sizeof e->name - 1] = 0;
        snprintf(e->path, sizeof e->path, "/tonetrix/backing/%s", fno.fname);
    }
    f_closedir(&dir);
}

int         backing_count(void)      { return s_count; }
const char *backing_name(int i)      { return (i >= 0 && i < s_count) ? s_files[i].name : ""; }
bool        backing_playing(void)    { return s_playing; }
int         backing_current(void)    { return s_playing ? s_cur : -1; }
float       backing_level(void)      { return s_level; }
void        backing_set_level(float g) { s_level = g < 0.0f ? 0.0f : (g > 2.0f ? 2.0f : g); }

// ------------------------------------------------------------ header parse

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }

// Walk the RIFF chunk list, filling the format + data-extent statics. Leaves the file
// positioned at the first data byte.
static int parse_header(void) {
    uint8_t hdr[12]; UINT br;
    if (f_read(&s_fil, hdr, 12, &br) != FR_OK || br != 12)            return BACKING_ERR_RIFF;
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))         return BACKING_ERR_RIFF;

    bool have_fmt = false;
    uint32_t pos = 12;
    for (;;) {
        uint8_t ch[8];
        if (f_lseek(&s_fil, pos) != FR_OK)                            return BACKING_ERR_DATA;
        if (f_read(&s_fil, ch, 8, &br) != FR_OK || br != 8)           return have_fmt ? BACKING_ERR_DATA
                                                                                      : BACKING_ERR_FMT;
        uint32_t id_off = pos + 8, sz = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[40];
            uint32_t want = sz > sizeof f ? sizeof f : sz;
            if (f_read(&s_fil, f, want, &br) != FR_OK || br != want)  return BACKING_ERR_FMT;
            uint16_t tag = rd16(f);
            s_chans = rd16(f + 2);
            s_rate  = rd32(f + 4);
            s_bits  = rd16(f + 14);
            if (tag == 0xFFFE && want >= 26) tag = rd16(f + 24);      // EXTENSIBLE: real sub-format
            s_float_fmt = (tag == 3);
            if (tag != 1 && tag != 3)                                 return BACKING_ERR_UNSUP;
            if (s_chans < 1 || s_chans > 2)                           return BACKING_ERR_UNSUP;
            if (s_bits != 8 && s_bits != 16 && s_bits != 24 && s_bits != 32) return BACKING_ERR_UNSUP;
            if (s_float_fmt && s_bits != 32)                          return BACKING_ERR_UNSUP;
            if (s_rate < 4000 || s_rate > 192000)                     return BACKING_ERR_UNSUP;
            have_fmt = true;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt)                                            return BACKING_ERR_FMT;
            s_data_start = id_off;
            s_data_bytes = sz;
            return f_lseek(&s_fil, s_data_start) == FR_OK ? 0 : BACKING_ERR_DATA;
        }
        pos = id_off + sz + (sz & 1);                                 // chunks are word-aligned
    }
}

// ------------------------------------------------------------- production

// Convert one frame's worth of raw bytes to a mono int16, averaging channels.
static inline int16_t frame_to_mono(const uint8_t *p) {
    int32_t acc = 0;
    for (int c = 0; c < s_chans; c++) {
        const uint8_t *q = p + c * (s_bits / 8);
        int32_t v;
        if (s_float_fmt) {
            float f; memcpy(&f, q, 4);
            v = (int32_t)(f * 32767.0f);
        } else switch (s_bits) {
            case 8:  v = ((int32_t)q[0] - 128) << 8;                              break;
            case 16: v = (int16_t)rd16(q);                                        break;
            case 24: v = (int32_t)((q[0]<<8) | (q[1]<<16) | ((uint32_t)q[2]<<24)) >> 16; break;
            default: v = (int32_t)(rd32(q)) >> 16;                                break;  // 32-bit int
        }
        acc += v;
    }
    acc /= s_chans;
    return (int16_t)(acc < -32768 ? -32768 : (acc > 32767 ? 32767 : acc));
}

// Read one bounded chunk into the ring. Returns false when nothing was read.
static bool fill_chunk(void) {
    const uint32_t fbytes = (uint32_t)s_chans * (s_bits / 8);
    uint32_t space = RING_SAMPLES - (s_head - s_tail);
    if (space < CHUNK_BYTES / fbytes + 2) return false;               // no room

    uint32_t left  = s_data_bytes - s_data_pos;
    uint32_t want  = CHUNK_BYTES;
    if (want > left) want = left;
    want -= want % fbytes;                                            // whole frames only

    if (want == 0) {                                                  // EOF → loop to the top
        if (f_lseek(&s_fil, s_data_start) != FR_OK) { s_playing = false; return false; }
        s_data_pos = 0;
        return true;                                                  // next call reads
    }
    UINT br = 0;
    if (f_read(&s_fil, s_raw, want, &br) != FR_OK || br == 0) { s_playing = false; return false; }
    s_data_pos += br;

    uint32_t frames = br / fbytes;
    for (uint32_t i = 0; i < frames; i++)
        s_ring[(s_head + i) & RING_MASK] = frame_to_mono(s_raw + i * fbytes);
    s_head += frames;
    return true;
}

void backing_service(void) {
    if (!s_playing) return;
    uint32_t t0 = time_us_32();
    // One chunk normally; up to four while the ring is below a quarter, so a track
    // that has just started (or just survived a stall) refills quickly. Still bounded.
    uint32_t fill = s_head - s_tail;
    int budget = (fill < RING_SAMPLES / 4) ? 4 : 1;
    while (budget-- > 0 && fill_chunk()) { }
    uint32_t dt = time_us_32() - t0;
    if (dt > s_max_service_us) s_max_service_us = dt;
}

// -------------------------------------------------------------- selection

int backing_play(int i) {
    if (i < 0 || i >= s_count) return BACKING_ERR_NOFILE;
    backing_stop();
    if (f_open(&s_fil, s_files[i].path, FA_READ) != FR_OK) return BACKING_ERR_OPEN;
    int rc = parse_header();
    if (rc != 0) { f_close(&s_fil); return rc; }

    s_head = s_tail = 0;
    s_phase = 0;
    s_phase_inc = ((uint64_t)s_rate << 32) / ENGINE_RATE;
    s_data_pos = 0;
    s_cur = i;
    s_playing = true;
    // Prime only an eighth of the ring (~8 kB). This read is BLOCKING and sits in the
    // foreground loop, so it stalls audio exactly like an IR switch does -- acceptable
    // for a deliberate user action, but worth keeping short. backing_service() bursts
    // 4 chunks/iteration while the ring is under a quarter, so it catches up in a few ms.
    for (int k = 0; k < 16 && (s_head - s_tail) < RING_SAMPLES / 8; k++) fill_chunk();
    return 0;
}

void backing_stop(void) {
    if (s_playing) { f_close(&s_fil); s_playing = false; }
    s_head = s_tail = 0; s_phase = 0; s_cur = -1;
}

// --------------------------------------------------------------- playback

void backing_mix(float *dst, int n) {
    if (!s_playing || s_level <= 0.0f) return;

    // Samples this block will consume, plus one for the interpolator's right-hand tap.
    uint64_t end = s_phase + (uint64_t)n * s_phase_inc;
    uint32_t need = (uint32_t)(end >> 32) + 2;
    if ((s_head - s_tail) < need) { s_underruns++; return; }          // guitar path untouched

    const float g = s_level * (1.0f / 32768.0f);
    uint64_t ph = s_phase;
    for (int i = 0; i < n; i++) {
        uint32_t idx  = (uint32_t)(ph >> 32);
        float    frac = (float)(uint32_t)(ph & 0xffffffffu) * (1.0f / 4294967296.0f);
        int16_t  a = s_ring[(s_tail + idx)     & RING_MASK];
        int16_t  b = s_ring[(s_tail + idx + 1) & RING_MASK];
        dst[i] += ((float)a + ((float)b - (float)a) * frac) * g;
        ph += s_phase_inc;
    }
    uint32_t consumed = (uint32_t)(ph >> 32);
    s_tail  += consumed;
    s_phase  = ph - ((uint64_t)consumed << 32);                       // keep only the fraction
}

// ------------------------------------------------------------ diagnostics

void backing_stats(uint32_t *under, int *pct, uint32_t *max_us) {
    if (under)  *under  = s_underruns;
    if (pct)    *pct    = s_playing ? (int)((s_head - s_tail) * 100u / RING_SAMPLES) : 0;
    if (max_us) *max_us = s_max_service_us;
}
void backing_stats_reset(void) { s_underruns = 0; s_max_service_us = 0; }

int backing_bench(int kb) {
    int idx = (s_cur >= 0) ? s_cur : 0;
    if (idx >= s_count) return 0;
    bool was = s_playing;
    backing_stop();

    FIL f;
    if (f_open(&f, s_files[idx].path, FA_READ) != FR_OK) return 0;
    uint32_t total = (uint32_t)kb * 1024u, done = 0;
    uint32_t t0 = time_us_32();
    while (done < total) {
        UINT br = 0;
        if (f_read(&f, s_raw, CHUNK_BYTES, &br) != FR_OK || br == 0) break;
        done += br;
    }
    uint32_t dt = time_us_32() - t0;
    f_close(&f);
    if (was) backing_play(idx);
    return dt ? (int)((uint64_t)done * 1000000u / dt / 1024u) : 0;
}
