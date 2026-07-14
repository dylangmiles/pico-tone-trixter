/*
 * es8388_test — ES8388 init + transparent audio passthrough.
 *
 * Guitar → TL072 → ES8388 LIN2 → PIO1 (i2s_in_slave) → copy → PIO0 (i2s_out) → ES8388 DAC → jack.
 *
 * ES8388 is I2S slave; Pico drives MCLK (GPIO 21), SCLK (GPIO 16), LRCLK (GPIO 17).
 * PIO1 reads DOUT (GPIO 12) by watching SCLK/LRCLK with wait-gpio — no PIO clock config needed.
 *
 * Init is two-phase to eliminate I2C/SCLK crosstalk:
 *   Phase 1 (SCLK quiesced): GPIO 16/17 driven LOW via SIO, config registers written with zero
 *                             interference on SDA/SCL.
 *   Phase 2 (SCLK running):  PIO0 resumed, es8388_adcpower_resync() triggers I2S sync via the
 *                             ADCPOWER 0xFF→0x00 transition while SCLK is live.
 *
 * Channel layout (ES8388 Left-Justified, LRCLK=0 = left):
 *   buf[j*2 + 0] = left  (guitar, LIN2)
 *   buf[j*2 + 1] = right (unused)
 *
 * Wiring (proto-board layout, post 2026-05-XX pin swap):
 *   ES8388 DVDD  → 3.3V           ES8388 MCLK  → GPIO 21 (100Ω series)
 *   ES8388 AVDD  → 3.3V           ES8388 SCLK  → GPIO 16
 *   ES8388 GND   → GND            ES8388 LRCLK → GPIO 17
 *   ES8388 SDA   → GPIO 14 (I²C1) ES8388 DIN   → GPIO 13
 *   ES8388 SCL   → GPIO 15 (I²C1) ES8388 DOUT  → GPIO 12
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "hardware/clocks.h"
#include "hardware/resets.h"
#include "hardware/pwm.h"
#include "i2s/i2s.h"
#include "i2s_out.pio.h"
#include "audio/es8388.h"
#include "i2s_in_slave.pio.h"
#include "FFTConvolver.h"
#include "TwoStageFFTConvolver.h"
// Both IRs are compiled in so presets can switch them at runtime. Each is a 48 kHz
// 2048-tap capture; the headers use per-file #pragma once and share the symbol names
// ir_samples / ir_num_samples, so each is included in its own namespace.
namespace ir_tw {
#include "audio/samples/ir_array_tanglewood.h"
}
namespace ir_gn {
#include "audio/samples/ir_array_garrison.h"
}
#include "audio/dsp_chain.h"
#include "audio/tuner.h"
#include "audio/oled.h"
#include "audio/menu.h"
#include "audio/app_hooks.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>            // atoi (UART 'pga' arg)

// Enable FPU Flush-to-Zero on the CALLING core: denormal floats (< ~1e-38) flush to
// 0 instead of taking the slow IEEE denormal path. The IR's long decaying tail
// produces tiny denormals as notes ring out; without FZ they stall the FFT enough to
// drop audio blocks — a signal-dependent glitch (only while notes decay, never in
// silence; unaffected by copy_to_ram). Flushed values are far below the noise floor,
// so it's inaudible. FPSCR is per-core, so call this on BOTH Core 0 and Core 1.
static inline void fpu_enable_flush_to_zero(void) {
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);   // FZ bit
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
}

// Timing instrumentation for the dropped-block hunt (reported by `stats`). Worst-case
// since boot, in microseconds: if maxProc approaches the 5333 us per-block budget,
// Core 0's block work overruns; if maxTail approaches the 10667 us 2-block window,
// Core 1's tail FFT is the bottleneck (Core 0 then blocks waiting for it).
static volatile uint32_t g_max_proc_us = 0;
static volatile uint32_t g_max_tail_us = 0;
static volatile uint32_t g_max_uart_us = 0;   // longest dsp_uart_poll() (runs every block)
static volatile uint32_t g_max_diag_us = 0;   // longest once-per-second diagnostics block
static volatile uint32_t g_max_gap_us  = 0;   // longest interval between consecutive block services

// ---------------------------------------------------------------------------
// IR convolution toggle.
//   0 = pure passthrough (original behaviour, baseline for A/B)
//   1 = FULL 2048-tap NT1-A IR convolution between ADC capture and DAC playback,
//       split across both cores (2026-06-03 migration): head on Core 0 (low
//       latency), tail on Core 1 (body resonance / reverb tail).
// ---------------------------------------------------------------------------
// Default build = full dual-core IR. The es8388_test_passthrough CMake target
// overrides this with -DENABLE_IR=0 for the no-convolver / no-Core-1 A/B baseline.
#ifndef ENABLE_IR
#define ENABLE_IR 1
#endif

// SD card (bit-bang SPI + FatFs) — product only; the codec bench (ENABLE_IR=0) omits the
// FatFs sources/include path, so keep these out of that translation unit.
#if ENABLE_IR
#include "audio/sd_spi.h"
#include "audio/wav_load.h"
#include "audio/tt_store.h"
#include <strings.h>          // strcasecmp
#include <cctype>             // tolower
extern "C" {                 // FatFs is a C library — keep C linkage in this C++ TU
#include "ff.h"
}
#endif

#if ENABLE_IR
// Full IR via TwoStageFFTConvolver:
//   - head (64-sample blocks) runs synchronously on Core 0 → low-latency direct sound
//   - tail (512-sample blocks) runs in the background on Core 1 → late reflections
// Replaces the old single-core 512-tap truncation: now the WHOLE 2048-tap IR, with
// latency still set by the 64-sample head (~1-2 ms of DSP), not a full extra block.
// Matches the CLAUDE.md perf projection (Core 0 head ~0.6 ms, Core 1 tail ~1.5 ms).
#define IR_HEAD_BLOCK   64
#define IR_TAIL_BLOCK   512

// Seeds the DSP chain's "out" stage (final output level). Headroom is handled up
// front by the chain's "in" trim (the IR adds ~25 dB on transients). K&K-slide gain
// staging: in.level 0.30 → EQ (+3 dB low boost) → comp → out.level 0.70 (pulled back
// from 1.0 to balance the EQ low boost, dialed 2026-06-08).
#define IR_OUTPUT_SCALE 0.7f

// --- Core 1 tail-convolution offload ---------------------------------------
// TwoStageFFTConvolver wraps its tail FFT in startBackgroundProcessing() /
// waitForBackgroundProcessing(). We override them to dispatch the tail work to
// Core 1 via two semaphores, so the heavy tail convolution never steals Core 0's
// real-time budget. The tail completes in ~2 ms but only fires every ~10.7 ms
// (every 2nd block), so Core 0's wait effectively never blocks.
static semaphore_t s_sem_tail_do;     // Core 0 → Core 1: a tail block is ready
static semaphore_t s_sem_tail_done;   // Core 1 → Core 0: tail block done (pre-released once)

class CoreOffloadConvolver : public fftconvolver::TwoStageFFTConvolver {
public:
    // Expose the protected base method so the Core 1 worker can drive it.
    void runBackgroundOnce() { doBackgroundProcessing(); }
protected:
    void startBackgroundProcessing() override { sem_release(&s_sem_tail_do); }
    void waitForBackgroundProcessing() override { sem_acquire_blocking(&s_sem_tail_done); }
};

static CoreOffloadConvolver s_convolver;

// Convolution runs in a Core 0 FOREGROUND loop, not in the input IRQ. The IRQ
// only captures + normalises a block into one of these double buffers and signals
// the foreground; keeping the IRQ light is what stops it delaying the output DMA
// refill (convolving inside the IRQ smeared the boot tone — 2026-06-05).
static float s_dsp_in [2][I2S_BLOCK_SIZE];  // double-buffered capture (IRQ writes, fg reads)
static float s_dsp_out[I2S_BLOCK_SIZE];
static semaphore_t s_sem_block_ready;       // input IRQ → foreground: a block is captured
static volatile int g_proc_idx = 0;         // which s_dsp_in buffer the fg should convolve

// IR table — index 0 is a synthetic "none" IR (convolution OFF); then the two embedded
// IRs; then any /tonetrix/ir/*.wav scanned off the SD card (ir_scan_sd). Presets AND the
// encoder IR picker index this one table, so "none" and SD IRs are all selectable like a
// built-in. SD entries carry a path and are decoded into s_sd_ir_buf at switch time.
#define SD_IR_MAX_TAPS 4096         // convolver scratch; >2048 fits Core 1's budget (bench-validated to 2048)
#define MAX_SD_IR      8
#define N_EMBED_IR     2            // real embedded IRs (tanglewood, garrison)
#define IR_NONE        0            // table index of the "no IR / convolution off" entry
#define IR_FIRST_REAL  1            // first real IR index (0 is "none")
struct IrEntry {
    const float *samples;   // embedded array; NULL for "none" and (until decoded) SD entries
    uint32_t     len;
    char         name[48];  // display / preset-match name
    char         path[80];  // SD file path (SD entries only)
    bool         is_sd;
};
static IrEntry s_ir_table[1 + N_EMBED_IR + MAX_SD_IR] = {
    { NULL,              0,                     "none",       "", false },  // [0] convolution off
    { ir_tw::ir_samples, ir_tw::ir_num_samples, "tanglewood", "", false },
    { ir_gn::ir_samples, ir_gn::ir_num_samples, "garrison",   "", false },
};
static int          s_ir_count   = 1 + N_EMBED_IR;   // none + 2 embedded; grows with the SD scan
static float        s_sd_ir_buf[SD_IR_MAX_TAPS];     // WAV decode scratch (convolver copies it in)
static int          s_cur_ir     = IR_FIRST_REAL;    // SELECTED IR (0=none); boot seeds tanglewood
static int          s_conv_ir    = IR_FIRST_REAL;    // which REAL IR is loaded in the convolver (never 0)
static volatile int g_pending_ir = -1;               // an IR (re)selection was requested; fg applies it
static int          s_cur_preset = 0;                // last-loaded preset (boot = 0); menu indicator
#define IR_TABLE_COUNT s_ir_count

// Case-insensitive name match, ignoring a trailing ".wav" on either side — so a preset
// `ir: tanglewood.wav` matches embedded "tanglewood", and `ir: mycab.wav` matches the
// scanned SD entry "mycab.wav".
static bool ir_name_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la >= 4 && strcasecmp(a + la - 4, ".wav") == 0) la -= 4;
    if (lb >= 4 && strcasecmp(b + lb - 4, ".wav") == 0) lb -= 4;
    if (la != lb) return false;
    for (size_t i = 0; i < la; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
// Resolve a preset's IR ref to a table index. "none"/"off"/empty → IR_NONE (dry). Otherwise
// SD entries win over embedded (card-first). -1 = unresolved (missing file / typo) ⇒ keep current.
static int resolve_ir_index(const char *name) {
    if (!name || !*name || strcasecmp(name, "none") == 0 || strcasecmp(name, "off") == 0) return IR_NONE;
    for (int i = IR_FIRST_REAL; i < s_ir_count; i++) if ( s_ir_table[i].is_sd && ir_name_eq(s_ir_table[i].name, name)) return i;
    for (int i = IR_FIRST_REAL; i < s_ir_count; i++) if (!s_ir_table[i].is_sd && ir_name_eq(s_ir_table[i].name, name)) return i;
    return -1;
}
// Scan /tonetrix/ir for *.wav and (re)build the SD portion of the IR table. The "none" +
// embedded entries [0..IR_FIRST_REAL+N_EMBED_IR) are kept; SD entries rebuilt each call.
static void ir_scan_sd(void) {
    s_ir_count = 1 + N_EMBED_IR;
    DIR dir;
    if (f_opendir(&dir, "/tonetrix/ir") != FR_OK) return;
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] && s_ir_count < 1 + N_EMBED_IR + MAX_SD_IR) {
        if (fno.fattrib & AM_DIR) continue;
        size_t l = strlen(fno.fname);
        if (l < 5 || strcasecmp(fno.fname + l - 4, ".wav") != 0) continue;
        IrEntry *e = &s_ir_table[s_ir_count++];
        e->samples = NULL; e->len = 0; e->is_sd = true;
        strncpy(e->name, fno.fname, sizeof e->name - 1); e->name[sizeof e->name - 1] = 0;
        snprintf(e->path, sizeof e->path, "/tonetrix/ir/%s", fno.fname);
    }
    f_closedir(&dir);
}

// --- app_hooks.h: bridge the menu (audio/) to preset + IR state owned here ---------
extern "C" {
int         app_preset_count(void)   { return dsp_chain_preset_count(); }
const char *app_preset_name(int i)   { return dsp_chain_preset_name(i); }
int         app_preset_current(void) { return s_cur_preset; }
void        app_preset_load(int i) {
    if (dsp_chain_load_preset(i) != 0) return;             // in/eq/comp/out params apply now
    int ir = resolve_ir_index(dsp_chain_preset_ir(i));     // IR_NONE / a real index / -1 (keep current)
    if (ir < 0) { /* preset IR missing → keep whatever's selected */ }
    else if (ir != s_cur_ir) g_pending_ir = ir;            // fg loads/decodes + sets convolution enable
    else dsp_chain_set_ir_enabled(ir != IR_NONE);          // same selection → just correct the enable
    int pdb = dsp_chain_preset_pga(i);                     // per-preset ES8388 PGA (dB); <0 = leave unchanged
    if (pdb >= 0) app_pga_set_nib((pdb + 1) / 3);          // K&K ~12, active Garrison ~6 — nearest 3 dB step
    s_cur_preset = i;
}
int         app_ir_count(void)       { return s_ir_count; }
const char *app_ir_name(int i)       { return (i >= 0 && i < s_ir_count) ? s_ir_table[i].name : "?"; }
int         app_ir_current(void)     { return g_pending_ir >= 0 ? g_pending_ir : s_cur_ir; }
void        app_ir_select(int i) {
    if (i >= 0 && i < s_ir_count && i != s_cur_ir) g_pending_ir = i;  // keep preset params
}
}

// 32 KB Core 1 stack — the tail FFT (1024-pt complex for a 512-sample block)
// needs ~20-30 KB of stack. Mirrors audio/dsp.cpp.
static uint32_t s_core1_stack[32768 / sizeof(uint32_t)];

// Core 1 entry: process tail blocks as Core 0 hands them off. Nothing else here.
static void tail_core1_entry(void) {
    fpu_enable_flush_to_zero();   // Core 1's FPU runs the tail FFT — flush denormals
    while (true) {
        sem_acquire_blocking(&s_sem_tail_do);
        uint32_t t0 = time_us_32();
        s_convolver.runBackgroundOnce();
        uint32_t dt = time_us_32() - t0;
        if (dt > g_max_tail_us) g_max_tail_us = dt;
        sem_release(&s_sem_tail_done);
    }
}

// Glitch instrumentation. g_irq1_count (input blocks captured) is defined with the
// IRQ counters further down — forward-declare it so `stats` can read it here.
extern volatile uint32_t g_irq1_count;
static volatile uint32_t g_dropped_blocks = 0;   // foreground skipped a captured block
static volatile bool     g_meter = false;        // live compressor gain-reduction print (~1/s)
static volatile bool     g_gr_oled = false;      // show the live GR-meter band on the home screen (menu / 'gr on')
static volatile bool     g_on_home = false;      // home/splash screen is the one currently displayed (not a menu)
static volatile bool     g_tuner = false;        // tuner mode: pitch-detect dry input, UART needle
static volatile uint32_t g_out_peak_q = 0;       // peak |output sample| (int32 mag) since last 'meter' read
static volatile uint32_t g_out_clip   = 0;       // # output samples that hit DAC full-scale since last 'meter' read
extern volatile uint32_t g_in_clip;              // ADC-input clip count — defined in the IRQ-visible block (with g_irq1_count)

// app_hooks.h: GR-meter band toggle (state owned here; the menu + UART 'gr on/off' drive it).
// The foreground loop's home tick redraws/hides the band from g_gr_oled, so this is a plain setter.
extern "C" bool app_gr_enabled(void)  { return g_gr_oled; }
extern "C" void app_gr_set(bool on)   { g_gr_oled = on; }

// app_hooks.h: ES8388 input PGA (reg 0x09, both channels). Nibble 0..8 = 0..+24 dB in 3 dB
// steps. Default 4 (+12 dB) tracks es8388_init's 0x44 for the OPA1642 op-amp daughter; use
// 6 (+18 dB) for the JFET source-follower daughter. Live via UART 'pga' or the MAIN menu —
// lets us A/B the two daughters without reflashing.
static int s_pga_nib = 4;                              // 4 = +12 dB, matches es8388_init (0x44)
extern "C" int  app_pga_nib(void)  { return s_pga_nib; }
extern "C" int  app_pga_db(void)   { return s_pga_nib * 3; }
extern "C" void app_pga_set_nib(int n) {
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    s_pga_nib = n;
    es8388_write_reg(i2c1, 0x09, (uint8_t)((n << 4) | n));   // MicAmpL | MicAmpR, both channels
}

// Print one tuner line: note + cents + an ASCII needle ([:] = in tune, # = pitch).
static void tuner_print_uart(void) {
    TunerResult r = tuner_result();
    if (!r.valid) { printf("tuner:  --   (listening)\n"); return; }
    char bar[22];
    for (int i = 0; i < 21; i++) bar[i] = '-';
    bar[10] = ':';                                       // in-tune center
    int pos = 10 + (int)lroundf(r.cents / 5.0f);          // -50..+50 cents -> 0..20
    if (pos < 0)  pos = 0;
    if (pos > 20) pos = 20;
    bar[pos] = '#';                                       // the needle
    bar[21] = '\0';
    const char *st = (r.cents > 5.0f) ? "SHARP" : (r.cents < -5.0f) ? "FLAT" : "IN TUNE";
    printf("%-2s%-2d %6.1f Hz  [%s] %+5.1fc  %s\n",
           r.name, r.octave, (double)r.freq_hz, bar, (double)r.cents, st);
}

// Render the tuner onto the OLED: big note+octave, frequency, a centre-zero needle
// bar (±50 cents), and FLAT/IN TUNE/SHARP. Only called in tuner mode where the audio
// output is muted, so the ~20 ms flush costs nothing audible.
static void tuner_draw_oled(void) {
    TunerResult r = tuner_result();
    oled_clear();
    oled_text(0, 0, "TUNER");
    if (!r.valid) {
        oled_text(28, 30, "-- listening --");
        oled_flush();
        return;
    }
    char s[16];
    snprintf(s, sizeof s, "%s%d", r.name, r.octave);
    oled_text2x(48, 12, s);                                  // big note, e.g. "E2"
    snprintf(s, sizeof s, "%.1f Hz", (double)r.freq_hz);
    oled_text(40, 34, s);

    int cx = 64 + (int)lroundf(r.cents * 1.2f);              // ±50c -> ±60 px about centre
    if (cx < 4)   cx = 4;
    if (cx > 123) cx = 123;
    for (int x = 4; x <= 123; x++) oled_pixel(x, 50, true);  // baseline
    for (int y = 46; y <= 54; y++) oled_pixel(64, y, true);  // centre (in-tune) tick
    for (int y = 44; y <= 56; y++) { oled_pixel(cx, y, true); oled_pixel(cx - 1, y, true); }  // needle

    const char *st = (r.cents > 5.0f) ? "SHARP" : (r.cents < -5.0f) ? "FLAT" : "IN TUNE";
    oled_text(0, 56, st);
    oled_flush();
}

// Home-screen GR-meter band lives on pages 5-7 (y 40-63). draw_home_gr_band() repaints
// only these pages, so the static info above (pages 0-4) never flickers.
#define HOME_GR_Y0      40
#define HOME_GR_PAGE0   5
#define HOME_GR_PAGE1   7

// Draw the live gain-reduction band (label + bar) into the framebuffer. Does NOT flush.
// Returns true if it actually repainted. `force` = always repaint (initial draw from
// show_splash); otherwise repaints ONLY when the value/bar visibly changed since last time.
// That change-gate is what silences the I2C bus when the meter is idle: with no signal the
// GR sits at 0, so we stop flushing → no periodic bus activity → no crosstalk into the
// analog input (the "jackhammer" heard when unplugged). Smoothed: fast attack, slow release.
static bool draw_home_gr_band(bool force) {
    static float gr_disp = 0.0f;                  // smoothed reduction, dB (<= 0)
    static int   last_fw  = -1;                    // last drawn bar width
    static int   last_v10 = 1 << 30;              // last drawn value (dB ×10)
    float gr = dsp_chain_comp_gr_db();            // peak reduction since last read, <= 0 dB
    if (gr < gr_disp) gr_disp = gr;              // fast attack (deeper reduction)
    else              gr_disp += (gr - gr_disp) * 0.45f;   // slow release toward current
    // Label + bar pushed toward the bottom for breathing room under the info block; bar is
    // half-height (6 px) and left-aligned to x=0 so its left edge lines up with the text.
    const int LBL_Y = 46;                        // "GR x.x dB" label row
    const int BX = 0, BY = 56, BW = 120, BH = 6; // bar box (BX=0 aligns with the label above)
    float frac = (-gr_disp) / 24.0f;             // full scale = 24 dB of reduction
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int fw  = (int)(frac * (BW - 2));
    int v10 = (int)lroundf(gr_disp * 10.0f);
    if (!force && fw == last_fw && v10 == last_v10) return false;   // nothing changed → no I2C
    last_fw = fw; last_v10 = v10;
    oled_fill_rect(0, HOME_GR_Y0, 128, 64 - HOME_GR_Y0, false);   // clear the band (pages 5-7)
    char v[20];
    snprintf(v, sizeof v, "GR %4.1f dB", (double)gr_disp);
    oled_text(0, LBL_Y, v);
    for (int x = BX; x < BX + BW; x++) { oled_pixel(x, BY, true); oled_pixel(x, BY + BH - 1, true); }
    for (int y = BY; y < BY + BH; y++) { oled_pixel(BX, y, true); oled_pixel(BX + BW - 1, y, true); }
    if (fw > 0) oled_fill_rect(BX + 1, BY + 1, fw, BH - 2, true);
    return true;
}

// The home / splash screen — static info (title, preset, bypass state) up top, plus the
// live GR-meter band at the bottom when enabled ('gr off' hides it). Shown at boot and
// whenever we leave a transient mode (tuner exit, bypass toggle) or after a menu-idle
// timeout. The full-frame flush is audio-safe (async DMA, serviced by the Core 0 loop) so
// even the automatic idle-return never blips the audio. Marks us as "on home" so the live
// meter tick knows it may repaint the band (and won't stomp a menu).
static void show_splash(void) {
    oled_clear();
    oled_text(0,  0, "Tone Trixter");
    oled_text(0, 16, "P: ");
    oled_text(18, 16, app_preset_name(app_preset_current()));
    oled_text(0, 30, g_dsp_bypass ? "-- BYPASS --" : "48 kHz IR ready");
    if (g_gr_oled) draw_home_gr_band(true);       // force the initial band draw
    oled_flush_async();
    g_on_home = true;
}

// OLED re-init + diagnostic pattern (`oled` command). Borders on the very top and
// bottom rows + TOP/BOT labels make any vertical offset/wrap instantly obvious — if the
// init misaligns, the borders won't sit at the screen edges.
static void oled_diag_pattern(void) {
    oled_clear();
    for (int x = 0; x < 128; x++) { oled_pixel(x, 0, true); oled_pixel(x, 63, true); }
    oled_text(2, 4,  "OLED TOP");
    oled_text(2, 28, "reinit ok");
    oled_text(2, 52, "OLED BOT");
    oled_flush();
}

// --- Bring-up debug helpers (I²C scan + encoder) ----------------------------
// Encoder pins per the GPIO map: A=GP4, B=GP3, SW=GP2 (internal pull-ups). Used only
// by the `enc` debug command for now; the real encoder driver/menu comes later.
#define ENC_A_PIN  4
#define ENC_B_PIN  3
#define ENC_SW_PIN 2
static volatile bool g_enc_dbg = false;
static volatile bool g_fsw_dbg = false;   // raw footswitch wiring test (see fsw_dbg_poll)

// Scan I²C1 (the ES8388 + OLED bus) and print every ACKing address.
static void i2c_scan_print(void) {
    printf("I2C1 scan:");
    int n = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        uint8_t rx;
        if (i2c_read_blocking(i2c1, a, &rx, 1, false) >= 0) { printf(" 0x%02X", a); n++; }
    }
    printf("  (%d found — expect 0x10 ES8388, 0x3C OLED)\n", n);
}

static void enc_dbg_init(void) {
    gpio_init(ENC_A_PIN);  gpio_set_dir(ENC_A_PIN,  GPIO_IN); gpio_pull_up(ENC_A_PIN);
    gpio_init(ENC_B_PIN);  gpio_set_dir(ENC_B_PIN,  GPIO_IN); gpio_pull_up(ENC_B_PIN);
    gpio_init(ENC_SW_PIN); gpio_set_dir(ENC_SW_PIN, GPIO_IN); gpio_pull_up(ENC_SW_PIN);
}

// Print encoder A/B/SW on any change — turn/press to verify wiring (`enc on|off`).
static void enc_dbg_poll(void) {
    static int la = -1, lb = -1, ls = -1;
    int a = gpio_get(ENC_A_PIN), b = gpio_get(ENC_B_PIN), s = gpio_get(ENC_SW_PIN);
    if (a != la || b != lb || s != ls) {
        printf("[enc] A=%d B=%d SW=%d\n", a, b, s);
        la = a; lb = b; ls = s;
    }
}

// Quadrature decoder: returns +1 (CW) / -1 (CCW) per detent (4 valid transitions),
// 0 otherwise. *clicked = debounced button press (falling edge, 200 ms lockout).
static int enc_read(bool *clicked) {
    static const int8_t QTAB[16] = { 0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0 };
    static uint8_t prev = 0x3;      // rest = A=1, B=1
    static int8_t  acc  = 0;
    int step = 0;
    uint8_t cur = (uint8_t)((gpio_get(ENC_A_PIN) << 1) | gpio_get(ENC_B_PIN));
    if (cur != prev) {
        acc += QTAB[(prev << 2) | cur];
        prev = cur;
        if (acc >= 4)       { acc = 0; step = +1; }
        else if (acc <= -4) { acc = 0; step = -1; }
    }
    static bool     last_sw = true;
    static uint32_t lock_sw = 0;
    bool sw = gpio_get(ENC_SW_PIN);
    uint32_t now = time_us_32();
    *clicked = (last_sw && !sw && (now - lock_sw) > 200000u);
    if (*clicked) lock_sw = now;
    last_sw = sw;
    return step;
}

// Drive the OLED menu from the encoder. The framebuffer flush is now DMA-backed and
// non-blocking (oled_flush_async + oled_flush_service in the Core 0 loop), so navigation
// repaints no longer hiccup the audio; it still only repaints on an actual encoder event.
// (Manual OLED reinit, if a panel ever garbles, is the UART `oled` command — the old
// double-click reinit gesture was retired now that the GP11 hardware RES keeps it stable.)
//
// After this long without an encoder event, drop out of the menu back to the home
// screen so the live GR meter reappears while you play.
#define HOME_IDLE_US  8000000u

static void menu_poll(void) {
    bool click;
    int turn = enc_read(&click);

    static uint32_t last_activity_us = 0;
    if (turn != 0 || click) last_activity_us = time_us_32();

    // On the home screen, a single encoder input OPENS the menu at "< back" — it doesn't act
    // as a menu command. So a click on home enters the menu, and the next click (on the
    // selected "< back") returns to home.
    if (g_on_home) {
        if (turn != 0 || click) {
            menu_open();
            menu_render();
            oled_flush_async();
            g_on_home = false;
        }
        return;
    }

    bool changed = (turn != 0 || click) && menu_event(turn, click);
    if (menu_take_home()) {
        show_splash();                // "< back" at MAIN → return to the home screen
    } else if (changed) {
        menu_render();
        oled_flush_async();
    } else if ((uint32_t)(time_us_32() - last_activity_us) > HOME_IDLE_US) {
        show_splash();                // menu idle → return home so the live meter resumes
    }
}

#if ENABLE_IR
// SD-card bring-up test (`sdtest`): init the card, mount FAT, list the root directory.
// Proves the bit-bang SPI + FatFs stack on hardware before wiring SD into IR loading.
// Blocking (fine — a deliberate bench command); reads glitch audio like any UART reply.
static FATFS s_fatfs;                              // static so the mount persists after sd_test()
static void sd_test(void) {
    printf("sd: init...\n");
    sd_set_verbose(true);                          // trace CMD0/CMD8/ACMD41 so we can see where it stops
    if (!sd_init()) { printf("sd: no card / init failed — check CS/SCK/MISO/MOSI + 3V3 rail\n"); return; }
    uint32_t sec = sd_sector_count();
    printf("sd: card ok — %s, %lu sectors (~%lu MB)\n",
           sd_is_sdhc() ? "SDHC/SDXC" : "SDSC", (unsigned long)sec, (unsigned long)(sec / 2048));
    FRESULT fr = f_mount(&s_fatfs, "", 1);         // mount now (opt 1 = mount immediately)
    if (fr != FR_OK) { printf("sd: f_mount failed (%d) — FAT32-formatted?\n", (int)fr); return; }
    DIR dir;
    FILINFO fno;
    if ((fr = f_opendir(&dir, "/")) != FR_OK) { printf("sd: opendir failed (%d)\n", (int)fr); return; }
    printf("sd: mounted. root dir:\n");
    int n = 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        printf("  %-13s %8lu B%s\n", fno.fname, (unsigned long)fno.fsize,
               (fno.fattrib & AM_DIR) ? "  <dir>" : "");
        n++;
    }
    f_closedir(&dir);
    printf("sd: %d entries. (8.3 names only — LFN off)\n", n);
}
#endif // ENABLE_IR

// Non-blocking UART command poll. Accumulates a line; on newline it dispatches to
// the DSP chain (which now owns the IR on/off flag too). getchar_timeout_us(0) never
// blocks, so polling between blocks can't drop audio the way a blocking printf would.
// (Command replies DO printf — a one-block blip per command, fine while tuning.)
// `stats` reports captured-vs-dropped block counts for glitch diagnosis.
static void dsp_uart_poll(void) {
    static char line[64];
    static int  n = 0;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            if (n > 0) {
                line[n] = '\0';
                if (strcmp(line, "preset") == 0 || strncmp(line, "preset ", 7) == 0) {
                    const char *arg = line + 6;
                    while (*arg == ' ') arg++;
                    if (*arg == '\0') {                  // list
                        printf("presets:");
                        for (int i = 0; i < dsp_chain_preset_count(); i++)
                            printf(" %s", dsp_chain_preset_name(i));
                        printf("   (IR now: %s)\n", s_ir_table[s_cur_ir].name);
                    } else {
                        int idx = dsp_chain_find_preset(arg);
                        if (idx < 0) { printf("? no preset '%s'\n", arg); }
                        else {
                            app_preset_load(idx);              // params now + safe IR switch + s_cur_preset
                            printf("preset '%s' loaded (IR %s)\n", dsp_chain_preset_name(idx),
                                   s_ir_table[app_ir_current()].name);
                        }
                    }
                }
                else if (strcmp(line, "ir") == 0 || strncmp(line, "ir ", 3) == 0) {
                    const char *arg = line + 2;
                    while (*arg == ' ') arg++;
                    if (*arg == '\0') {                        // show current + list options
                        printf("IR: '%s'%s\n  options:", s_ir_table[s_cur_ir].name,
                               s_cur_ir == IR_NONE ? " (convolution off)" : "");
                        for (int i = 0; i < s_ir_count; i++) printf(" %s", s_ir_table[i].name);
                        printf("\n  usage: ir <name> | ir none | ir on\n");
                    } else if (strcmp(arg, "on") == 0) {       // re-engage the last real IR
                        int t = (s_cur_ir == IR_NONE) ? s_conv_ir : s_cur_ir;
                        app_ir_select(t);
                        printf("ir on -> %s\n", s_ir_table[t].name);
                    } else {                                    // "none"/"off" or an IR name
                        int idx = resolve_ir_index(arg);
                        if (idx < 0) {
                            printf("? no IR '%s'. options:", arg);
                            for (int i = 0; i < s_ir_count; i++) printf(" %s", s_ir_table[i].name);
                            printf("\n");
                        } else {
                            app_ir_select(idx);
                            printf("ir -> %s%s\n", s_ir_table[idx].name,
                                   idx == IR_NONE ? " (convolution off)" : "");
                        }
                    }
                }
                else if (strcmp(line, "tuner on") == 0)  { g_tuner = true;  printf("tuner=on (dry monitor; 'tuner off' to resume)\n"); }
                else if (strcmp(line, "tuner off") == 0) { g_tuner = false; printf("tuner=off\n"); }
                else if (strcmp(line, "meter on") == 0)  { g_out_peak_q = 0; g_out_clip = 0; g_in_clip = 0; g_meter = true;  printf("meter=on (comp in / GR / output dBFS / ADC+DAC clip, ~1/s)\n"); }
                else if (strcmp(line, "meter off") == 0) { g_meter = false; printf("meter=off\n"); }
                else if (strcmp(line, "gr on") == 0)  { g_gr_oled = true;  if (g_on_home && !g_tuner) show_splash(); printf("gr=on (home GR-meter band shown)\n"); }
                else if (strcmp(line, "gr off") == 0) { g_gr_oled = false; if (g_on_home && !g_tuner) show_splash(); printf("gr=off (home GR-meter band hidden)\n"); }
                else if (strcmp(line, "pga") == 0 || strncmp(line, "pga ", 4) == 0) {
                    // ES8388 input PGA gain (reg 0x09) — live, no reflash. The PGA is
                    // quantised to 3 dB steps (0,3,..,24); a requested dB snaps to the
                    // nearest. +12 dB = op-amp daughter, +18 dB = JFET daughter.
                    const char *arg = line + 3;
                    while (*arg == ' ') arg++;
                    if (*arg == '\0')
                        printf("pga: +%d dB (step %d/8). usage: pga <dB> — snaps to nearest 3 dB step "
                               "(0,3,6,9,12,15,18,21,24); +12=op-amp, +18=JFET\n",
                               app_pga_db(), app_pga_nib());
                    else {
                        int req = atoi(arg);                       // "12" or "+12"
                        app_pga_set_nib((req + 1) / 3);            // nearest 3 dB step; hook clamps 0..8
                        const char *tag = app_pga_nib() == 4 ? "  [op-amp daughter]" :
                                          app_pga_nib() == 6 ? "  [JFET daughter]"   : "";
                        if (app_pga_db() != req)
                            printf("pga -> +%d dB (step %d/8; snapped from %d to nearest 3 dB step)%s\n",
                                   app_pga_db(), app_pga_nib(), req, tag);
                        else
                            printf("pga -> +%d dB (step %d/8)%s\n", app_pga_db(), app_pga_nib(), tag);
                    }
                }
                else if (strcmp(line, "oleddma") == 0) oled_dma_selftest();   // DMA-flush diagnostic
                else if (strcmp(line, "stats") == 0)
                    printf("blocks=%lu dropped=%lu proc=%lu tail=%lu uart=%lu diag=%lu gap=%lu us\n",
                           (unsigned long)g_irq1_count, (unsigned long)g_dropped_blocks,
                           (unsigned long)g_max_proc_us, (unsigned long)g_max_tail_us,
                           (unsigned long)g_max_uart_us, (unsigned long)g_max_diag_us,
                           (unsigned long)g_max_gap_us);
                else if (strcmp(line, "i2cscan") == 0) i2c_scan_print();
#if ENABLE_IR
                else if (strcmp(line, "sdtest") == 0)  sd_test();       // SD bring-up: init + mount + list root
                else if (strcmp(line, "sdpins") == 0)  sd_pin_check();  // float/short test (disconnect module)
                else if (strcmp(line, "sdcfg") == 0)   tt_store_dump(); // loaded on-card config/presets
                else if (strcmp(line, "sdir") == 0) {                   // IR table (built-in + scanned SD WAVs)
                    printf("IR table (%d):\n", s_ir_count);
                    for (int i = 0; i < s_ir_count; i++)
                        printf("  [%d]%c %-20s %s\n", i, i == s_cur_ir ? '*' : ' ', s_ir_table[i].name,
                               i == IR_NONE ? "(convolution off)" : s_ir_table[i].is_sd ? s_ir_table[i].path : "(built-in)");
                }
                else if (strcmp(line, "sdreload") == 0) {               // re-mount + re-read card without a reboot
                    if (sd_init() && f_mount(&s_fatfs, "", 1) == FR_OK) {
                        ir_scan_sd();
                        int n = 0;
                        if (tt_store_load()) { const Preset *ps = tt_store_presets(&n); dsp_chain_install_presets(ps, n); }
                        else                   dsp_chain_install_presets(NULL, 0);   // revert to built-ins
                        printf("sdreload: %d preset%s, %d SD IR%s\n", n, n == 1 ? "" : "s",
                               s_ir_count - N_EMBED_IR, (s_ir_count - N_EMBED_IR) == 1 ? "" : "s");
                    } else printf("sdreload: no card / mount failed\n");
                }
#endif
                else if (strcmp(line, "enc on") == 0)  { g_enc_dbg = true;  printf("enc=on (turn/press to see A/B/SW)\n"); }
                else if (strcmp(line, "enc off") == 0) { g_enc_dbg = false; printf("enc=off\n"); }
                else if (strcmp(line, "fsw on") == 0)  { g_fsw_dbg = true;  printf("fsw=on (stomp to see GP18/GP19; no mode toggle)\n"); }
                else if (strcmp(line, "fsw off") == 0) { g_fsw_dbg = false; printf("fsw=off\n"); }
                else if (strcmp(line, "oled") == 0) {
                    bool ok = oled_init();                 // re-run init; retry a bad/offset boot live
                    if (ok) oled_diag_pattern();
                    printf("oled reinit: %s\n", ok ? "ok — TOP/BOT border pattern (check edges)" : "no ACK");
                }
                else if (strcmp(line, "help") == 0) {
                    // Bring-up / test commands live in main.cpp; DSP commands in the chain.
                    printf("Bring-up / test:\n"
                           "  i2cscan                 scan I2C1 (expect 0x10 ES8388, 0x3C OLED)\n"
                           "  enc on|off              raw encoder A/B/SW on change (wiring test)\n"
                           "  fsw on|off              raw footswitch GP18/GP19 on change (wiring test)\n"
                           "  oled                    re-init OLED + TOP/BOT border test pattern\n"
                           "  oleddma                 async OLED DMA-flush self-test\n"
                           "  gr on|off               live OLED gain-reduction meter (filming/bench; blips audio)\n"
                           "  pga <dB>                ES8388 input PGA (0..24, 3 dB steps; +12 op-amp, +18 JFET daughter)\n");
#if ENABLE_IR
                    printf("SD card (/tonetrix on the card):\n"
                           "  sdtest                  init + mount + list root dir\n"
                           "  sdpins                  GP6/8/9/10 float/short test (disconnect module first)\n"
                           "  sdcfg                   show on-card config + presets that were loaded\n"
                           "  sdir                    list IR table (built-in + scanned SD WAVs; * = current)\n"
                           "  sdreload                re-read the card after editing (no reboot)\n");
#endif
                    dsp_chain_command(line);               // then the DSP-command section
                }
                else if (strcmp(line, "dump") == 0) {
                    // App/codec settings owned here (not chain params), then the chain dump.
                    printf("--- app/codec ---\n");
                    printf("  pga        = +%d dB (step %d/8)%s\n", app_pga_db(), app_pga_nib(),
                           app_pga_nib() == 4 ? "  [op-amp daughter]" :
                           app_pga_nib() == 6 ? "  [JFET daughter]"   : "");
                    printf("  preset     = %s [%d]\n", dsp_chain_preset_name(s_cur_preset), s_cur_preset);
                    printf("  ir         = %s%s\n", s_ir_table[app_ir_current()].name,
                           app_ir_current() == IR_NONE ? " (convolution off)" : "");
                    printf("  gr_meter   = %s\n", g_gr_oled ? "on" : "off");
                    printf("  tuner      = %s\n", g_tuner  ? "on" : "off");
                    printf("  meter      = %s\n", g_meter  ? "on" : "off");
                    dsp_chain_command(line);               // then the DSP chain (stages + params + bypass)
                }
                else if (!dsp_chain_command(line))
                    printf("? '%s' (try help)\n", line);
                n = 0;
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

// --- Footswitches -----------------------------------------------------------
// Two soft (momentary SPST, normally-open) footswitches wired pin→GND, read with
// the Pico's internal pull-ups: FSW_TUNER toggles tuner mode, FSW_BYPASS toggles
// DSP bypass. Active-low; an UNCONNECTED pin floats HIGH (pulled up) = "released",
// so this is safe to run before the switches are wired. Debounce = falling-edge
// detect + 200 ms lockout. Pins are free GPIOs — change to match your wiring.
#define FSW_TUNER_PIN   18
#define FSW_BYPASS_PIN  19

static void footswitch_init(void) {
    gpio_init(FSW_TUNER_PIN);   gpio_set_dir(FSW_TUNER_PIN,  GPIO_IN);  gpio_pull_up(FSW_TUNER_PIN);
    gpio_init(FSW_BYPASS_PIN);  gpio_set_dir(FSW_BYPASS_PIN, GPIO_IN);  gpio_pull_up(FSW_BYPASS_PIN);
}

// Raw footswitch state on any change — stomp to verify wiring (`fsw on|off`).
// Prints the live pin levels WITHOUT toggling tuner/bypass, so it's a clean wiring
// test: a correctly-wired switch reads 1 (released) and drops to 0 while held.
static void fsw_dbg_poll(void) {
    static int lt = -1, lb = -1;
    int t = gpio_get(FSW_TUNER_PIN), b = gpio_get(FSW_BYPASS_PIN);
    if (t != lt || b != lb) {
        printf("[fsw] GP18 tuner=%d  GP19 bypass=%d  (0 = pressed)\n", t, b);
        lt = t; lb = b;
    }
}

static void footswitch_poll(void) {
    static bool     last_t = true, last_b = true;   // pulled-up HIGH = released
    static uint32_t lock_t = 0,    lock_b = 0;
    uint32_t now = time_us_32();
    bool t = gpio_get(FSW_TUNER_PIN);
    if (last_t && !t && (now - lock_t) > 200000u) {   // press (falling edge) + 200 ms debounce
        g_tuner = !g_tuner;  lock_t = now;
        printf("[fsw] tuner=%s\n", g_tuner ? "on" : "off");
    }
    last_t = t;
    bool b = gpio_get(FSW_BYPASS_PIN);
    if (last_b && !b && (now - lock_b) > 200000u) {
        g_dsp_bypass = !g_dsp_bypass;  lock_b = now;
        printf("[fsw] bypass=%s\n", g_dsp_bypass ? "on" : "off");
    }
    last_b = b;
}
#endif

#define MCLK_PIN        21
#define ES8388_DOUT_PIN 12   /* GP12 — I²S RX from ES8388 ADC. Adjacent to DIN (GP13) for tidy bundle. */

// Cascade-debug scope trigger: goes HIGH the instant sync-loss is detected so
// the scope can single-trigger with pre-trigger buffer showing pre-cascade state.
#define CASCADE_TRIG_PIN 22  /* GP22 — debug-only, separate from audio pin block. */

// 1 kHz PWM test tone on GPIO 2. Set to 0 when feeding real signal (e.g. piezo
// via TL072) into LIN2 — leaving PWM running can couple into LIN2 through the
// attenuator network and nearby breadboard rails.
#define ENABLE_PWM_TEST  0

// Scope sentinel mode: when non-zero, every output sample is replaced with this
// constant (skipping boot tone and passthrough). Makes DIN a deterministic
// per-slot bit pattern for scoping LRCK/SCLK/DIN timing relationships.
//   0x80000000 → only sign bit set → 1-BCLK pulse per slot at MSB position
//   0xC0000000 → sign + bit 30 set → 2-BCLK pulse per slot
//   0           → disabled (normal passthrough_cb behavior)
#define SCOPE_SENTINEL_VALUE  0u

// Per-second "live pkL=... " UART print. MUST be 0 for clean audio with IR enabled:
// the convolution now runs in the Core 0 foreground loop, and a blocking ~4 ms UART
// printf there drops one audio block → a "dull scratch" blip once per second that
// scales with playing volume (2026-06-05). Sync-loss dumps still print regardless.
// Set to 1 only for bench telemetry when you can tolerate the periodic blip.
#define DEBUG_LIVE_PRINT  0

// ---- Buffers ---------------------------------------------------------------
static int32_t s_in_buf[2][I2S_BLOCK_SIZE * 2];
static int32_t s_out_buf[2][I2S_BLOCK_SIZE * 2];
// Double-buffered, published by index. With IR enabled the producer is the Core 0
// foreground (preemptible), so the output IRQ could otherwise read a half-written
// buffer (tearing). The producer fills the non-published buffer then flips
// g_staging_pub atomically; the output IRQ always reads a complete buffer.
// (ENABLE_IR=0 passthrough keeps writing buffer 0 with g_staging_pub==0 — the
// input IRQ and output IRQ never preempt each other, so no tearing there.)
static int32_t      s_staging_buf[2][I2S_BLOCK_SIZE * 2];
static volatile int g_staging_pub = 0;

static int  s_in_chan[2];
static PIO  s_in_pio;
static uint s_in_sm;

volatile uint32_t g_irq1_count  = 0;
volatile uint32_t g_stale_count = 0;
volatile uint32_t g_in_clip     = 0;   // ADC-input (guitar) samples at full-scale since last 'meter' read (IRQ-updated)
volatile int32_t  g_peak_l      = 0;
volatile int32_t  g_peak_r      = 0;
volatile int32_t  g_min_l       = 0;
volatile int32_t  g_max_l       = 0;

// Test tone shape:
//   0 = sine (production)
//   1 = triangle (graded sentinel)
//   2 = DC step test: +half-FS for 2 s, -half-FS for 2 s. (DC-couple scope.)
//   3 = ~200 Hz square from constants. Alternates +half-FS / -half-FS every 120 samples.
//   4 = 440 Hz sawtooth: single ramp -peak → +peak over the whole period, sharp drop back.
//       Splits "any smooth ramp distorts" vs "specifically two-ramp triangle distorts".
#define BOOT_TONE_SHAPE 1

// Sample format experiment (2026-04-26): if 1, XOR sign bit on each I2S sample
// to convert 2's complement → offset binary. Tests whether the V/Λ fold-back
// artifact comes from chip interpreting our 2's-comp data as offset binary.
#define BOOT_TONE_OFFSET_BINARY 0
static int32_t  s_sine_table[256];
static uint32_t g_tone_phase  = 0;
static uint32_t g_tone_blocks = 0;
#define TONE_DURATION_BLOCKS 1504u  // ~4 s at 376 blocks/s (96 kHz / 256 samples)
#define TONE_PHASE_INC       19685120u // 440 Hz at 96 kHz: 2^32 * 440 / 96000
#define TONE_HALF_BLOCKS     752u   // half of TONE_DURATION_BLOCKS, for DC-step test
#define SQUARE_HALF_SAMPLES  240u   // 96000 / (2 * 240) = 200 Hz square period

// ---------------------------------------------------------------------------
// SCLK quiesce / resume
//
// quiesce: stop PIO0 SM0 and drive GPIO 16 (SCLK) and GPIO 17 (LRCLK) LOW
//          via SIO so there is zero signal on those pins during I2C operations.
// resume:  hand the pins back to PIO0 and re-enable SM0.
// ---------------------------------------------------------------------------
static void sclk_quiesce(void) {
    pio_sm_set_enabled(pio0, 0, false);
    // Take ownership of SCLK and LRCLK as plain outputs, drive them LOW.
    gpio_init(I2S_BCLK_PIN);
    gpio_set_dir(I2S_BCLK_PIN, GPIO_OUT);
    gpio_put(I2S_BCLK_PIN, 0);
    gpio_init(I2S_BCLK_PIN + 1);
    gpio_set_dir(I2S_BCLK_PIN + 1, GPIO_OUT);
    gpio_put(I2S_BCLK_PIN + 1, 0);
    sleep_us(200);  // let any ringing settle
}

// PC start-position sweep (Path B option 2). 0 = no jmp (Phase 1 take 2 baseline,
// ~30% small-clean random). 1-4 select the four valid PC positions where X is
// initialised correctly for the next loop. Cycle through 1..4 by re-flashing.
//   1 = entry_point        (offset 15) — TESTED at +8: deterministic L-tiny-tone
//   2 = start_after_pre    (offset 3)  — mid-HIGH word, LRCK=1
//   3 = start_after_high_main (offset 7)  — start of LOW word, LRCK=1
//   4 = start_after_delay_high (offset 11) — mid-LOW word, LRCK=0
#define PIO_JMP_TARGET 2

static void sclk_resume(void) {
    pio_gpio_init(pio0, I2S_BCLK_PIN);
    pio_gpio_init(pio0, I2S_BCLK_PIN + 1);
    pio_sm_restart(pio0, 0);
    pio_sm_clkdiv_restart(pio0, 0);
#if PIO_JMP_TARGET != 0
    uint pc;
  #if PIO_JMP_TARGET == 1
    pc = i2s_pio_offset() + i2s_out_offset_entry_point;
  #elif PIO_JMP_TARGET == 2
    pc = i2s_pio_offset() + i2s_out_offset_start_after_pre;
  #elif PIO_JMP_TARGET == 3
    pc = i2s_pio_offset() + i2s_out_offset_start_after_high_main;
  #elif PIO_JMP_TARGET == 4
    pc = i2s_pio_offset() + i2s_out_offset_start_after_delay_high;
  #else
    #error "PIO_JMP_TARGET must be 0..4"
  #endif
    pio_sm_exec(pio0, 0, pio_encode_jmp(pc));
#endif
    pio_sm_set_enabled(pio0, 0, true);
}

// ---------------------------------------------------------------------------
// ES8388 sync helpers (defined after sclk_resume)
// ---------------------------------------------------------------------------
// Path B Phase 2 takes 1-6 all produced the same L-tiny-tone / R-loud-noise
// regime regardless of: ADCPOWER timing (0/5 µs delay, falling/rising edge,
// MASTERMODE include/exclude), CHIPPOWER trigger position (pre/post sclk_
// resume), or DACCONTROL1 format (DSP/Philips). The chip's lock phase is
// not controllable through register-write timing — it's determined entirely
// by clock relationships at sclk_resume.
//
// Reverted to original sync_cycle. Phase 1 (clkdiv_restart) is the only
// active Path B change. Now sweeping PIO LRCK delay (+8 / +11 / etc) since
// it's the only knob that demonstrably shifts the chip's lock phase
// (previously +9 → tiny tone, +10 → silent).
static void es8388_sync_cycle(i2c_inst_t *i2c) {
    es8388_chippower_cycle(i2c);
    es8388_config_only(i2c);             // re-apply all config after chippower reset
    es8388_write_reg(i2c, 0x03, 0x00);  // ADCPOWER=0x00 while quiesced (reliable)
    sclk_resume();
    es8388_adcpower_resync(i2c);         // MASTERMODE re-latch + ADCPOWER confirm
    sleep_ms(10);
}

// ---------------------------------------------------------------------------
// DMA_IRQ_1: input block complete. Copy left channel (guitar) to both outputs.
// ---------------------------------------------------------------------------
static void input_dma_irq1_handler(void) {
    g_irq1_count++;
#if ENABLE_IR
    const bool mtr = g_meter;   // read once — gates the ADC-clip meter work so it's free when 'meter' off
                                // (g_meter is product-only, inside the ENABLE_IR block; the meter is too)
#endif
    for (int i = 0; i < 2; i++) {
        if (!dma_channel_get_irq1_status(s_in_chan[i])) continue;
        dma_channel_acknowledge_irq1(s_in_chan[i]);

        const int32_t *src = s_in_buf[i];
        int32_t peak_l = 0, peak_r = 0, min_l = 0x7fffffff, max_l = (int32_t)0x80000000;
#if ENABLE_IR
        uint32_t blk_iclip = 0;               // ADC-input (left/guitar) samples at full-scale this block ('meter')
#endif
#if ENABLE_IR
        // Capture into the next double buffer; the foreground loop convolves it.
        static int s_dsp_w = 0;
        float *cap = s_dsp_in[s_dsp_w];
#endif
        for (int j = 0; j < I2S_BLOCK_SIZE; j++) {
            // << 1 is sign-recovery, not amplitude. PIO empirically captures a
            // leading "delay zero" in bit 31 with audio bits 31..1 in bits 30..0;
            // shift-left puts the original sign bit back into bit 31. Without
            // this shift, negative samples read as huge positive values
            // (rectification → 1.66 V pk-pk sawtooth at idle, 2026-04-27).
            int32_t l_raw = src[j * 2]     << 1;  // left  (LRCLK=0 = even slots)
            int32_t r_raw = src[j * 2 + 1] << 1;  // right (LRCLK=1 = odd slots)

#if ENABLE_IR
            // Normalise int32 → float [-1, 1) only. The convolution + scale + clip
            // happens in the Core 0 FOREGROUND loop (see main), NOT here — keeping
            // this IRQ light is what stops it delaying the output DMA refill.
            // Peak/min/max track the RAW INPUT for the [cal] diagnostic.
            cap[j] = (float)l_raw * (1.0f / 2147483648.0f);
#else
            // Original passthrough. Digital gain ×1 (no boost). 2026-05-10 LOUT2 →
            // LOUT1 hardware rewire put the HP amp's 6–10 dB gain into the chain;
            // analog +18 dB (ADCCONTROL1=0x66) + digital ×1 + LOUT1 amp = +12 dB
            // chain gain at TSout (verified 2026-05-16 + 2026-05-29 SNR sessions).
            int32_t l = l_raw;
            s_staging_buf[0][j * 2]     = l;
            s_staging_buf[0][j * 2 + 1] = l;
#endif

            int32_t al = l_raw < 0 ? -l_raw : l_raw;
            int32_t ar = r_raw < 0 ? -r_raw : r_raw;
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
#if ENABLE_IR
            if (mtr && al >= (int32_t)0x7FFF0000) blk_iclip++;   // ADC left-channel at full-scale (~-0.0003 dBFS) = clip (metering only)
#endif
            if (l_raw < min_l) min_l = l_raw;
            if (l_raw > max_l) max_l = l_raw;
        }
#if ENABLE_IR
        if (mtr) g_in_clip += blk_iclip;   // commit only while metering; read+reset by 'meter'
#endif

#if ENABLE_IR
        // Publish the captured block to the foreground and flip buffers. No
        // convolution in the IRQ — that is what kept the output DMA on time and
        // the boot tone clean (2026-06-05).
        g_proc_idx = s_dsp_w;
        s_dsp_w ^= 1;
        sem_release(&s_sem_block_ready);
#endif

        g_peak_l = peak_l;
        g_peak_r = peak_r;
        g_min_l  = min_l;
        g_max_l  = max_l;

        dma_channel_set_write_addr(s_in_chan[i], s_in_buf[i], false);
        dma_channel_set_trans_count(s_in_chan[i], I2S_BLOCK_SIZE * 2, false);
    }
}

// ---------------------------------------------------------------------------
// Output DMA callback (DMA_IRQ_0, from i2s.c).
// ---------------------------------------------------------------------------
static void passthrough_cb(int32_t *buf_done) {
#if SCOPE_SENTINEL_VALUE
    // Stuff every sample with the sentinel — DIN becomes a known per-slot pattern.
    const int32_t v = (int32_t)SCOPE_SENTINEL_VALUE;
    for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
        buf_done[i * 2]     = v;
        buf_done[i * 2 + 1] = v;
    }
    return;
#endif
    if (g_tone_blocks < TONE_DURATION_BLOCKS) {
#if BOOT_TONE_SHAPE == 2
        // DC step test: +half-FS for first 2 s, -half-FS for second 2 s.
        const int32_t v_dc = (g_tone_blocks < TONE_HALF_BLOCKS)
                             ? (int32_t)0x40000000 : (int32_t)0xC0000000;
        for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
            buf_done[i * 2]     = v_dc;
            buf_done[i * 2 + 1] = v_dc;
        }
#elif BOOT_TONE_SHAPE == 3
        // ~200 Hz square from alternating constants.
        for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
            int32_t v = ((g_tone_phase / SQUARE_HALF_SAMPLES) & 1u)
                        ? (int32_t)0xC0000000 : (int32_t)0x40000000;
            g_tone_phase++;
            buf_done[i * 2]     = v;
            buf_done[i * 2 + 1] = v;
        }
#elif BOOT_TONE_SHAPE == 4
        // 440 Hz sawtooth, 50% FS: single ramp -peak → +peak over period, sharp wrap.
        // phase >> 1 maps 32-bit phase to [0, 0x7FFFFFFF]; subtract 0x40000000 → [-0x40000000, +0x40000000).
        for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
            int32_t v = (int32_t)(g_tone_phase >> 1) - (int32_t)0x40000000;
            g_tone_phase += TONE_PHASE_INC;
#if BOOT_TONE_OFFSET_BINARY
            v ^= (int32_t)0x80000000;  // 2's complement → offset binary (sign bit flip)
#endif
            buf_done[i * 2]     = v;
            buf_done[i * 2 + 1] = v;
        }
#else
        for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
            int32_t v = s_sine_table[g_tone_phase >> 24];
            g_tone_phase += TONE_PHASE_INC;
#if BOOT_TONE_OFFSET_BINARY
            v ^= (int32_t)0x80000000;  // 2's complement → offset binary (sign bit flip)
#endif
            buf_done[i * 2]     = v;
            buf_done[i * 2 + 1] = v;
        }
#endif
        g_tone_blocks++;
        return;
    }
    int idx = (buf_done == s_out_buf[0]) ? 0 : 1;
    if (g_irq1_count == 0) {
        __builtin_memset(s_out_buf[idx], 0, I2S_BLOCK_SIZE * 2 * sizeof(int32_t));
        g_stale_count++;
        return;
    }
    // Read the most recently PUBLISHED staging buffer — always a complete block,
    // even if the foreground producer is mid-write on the other buffer.
    __builtin_memcpy(s_out_buf[idx], s_staging_buf[g_staging_pub], I2S_BLOCK_SIZE * 2 * sizeof(int32_t));
}

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------
static void i2c_bus_recover(void) {
    reset_block(RESETS_RESET_I2C1_BITS);
    unreset_block_wait(RESETS_RESET_I2C1_BITS);
    gpio_init(ES8388_SDA_PIN); gpio_set_dir(ES8388_SDA_PIN, GPIO_IN); gpio_pull_up(ES8388_SDA_PIN);
    gpio_init(ES8388_SCL_PIN); gpio_set_dir(ES8388_SCL_PIN, GPIO_OUT); gpio_put(ES8388_SCL_PIN, 1);
    sleep_us(100);
    for (int i = 0; i < 9; i++) {
        gpio_put(ES8388_SCL_PIN, 0); sleep_us(10);
        gpio_put(ES8388_SCL_PIN, 1); sleep_us(10);
        if (gpio_get(ES8388_SDA_PIN)) break;
    }
    gpio_set_dir(ES8388_SDA_PIN, GPIO_OUT); gpio_put(ES8388_SDA_PIN, 0);
    sleep_us(10); gpio_put(ES8388_SCL_PIN, 1); sleep_us(10); gpio_put(ES8388_SDA_PIN, 1);
    sleep_ms(5);
}

static void i2c_setup(void) {
    i2c_init(i2c1, 50000);
    gpio_set_function(ES8388_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(ES8388_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(ES8388_SDA_PIN);
    gpio_pull_up(ES8388_SCL_PIN);
    // Spike filter: 50 cycles @ 125 MHz = 400 ns > SCLK half-period 163 ns.
    // Filters SCLK glitches from coupling; should be zero-noise when SCLK is quiesced.
    i2c_get_hw(i2c1)->fs_spklen = 50;
}

// ---------------------------------------------------------------------------
// Register-dump helper for cascade debug — read the registers most likely to
// explain a silent internal mute (ALC, NGATE, DAC mute, power, volumes).
// ---------------------------------------------------------------------------
static const struct {
    uint8_t     addr;
    const char *name;
} DUMP_REGS[] = {
    {0x02, "CHIPPOWER"},
    {0x03, "ADCPOWER"},
    {0x04, "DACPOWER"},
    {0x08, "MASTERMODE"},
    {0x09, "MICAMP"},
    {0x0C, "ADCCTRL4"},
    {0x10, "LADCVOL"},
    {0x11, "RADCVOL"},
    {0x12, "ALC1"},
    {0x13, "ALC2"},
    {0x14, "ALC3"},
    {0x15, "ALC4"},
    {0x16, "NGATE"},
    {0x19, "DACCTRL3"},
};

static void dump_regs(i2c_inst_t *i2c, const char *label) {
    printf("  regs @ %s:", label);
    for (size_t i = 0; i < sizeof(DUMP_REGS) / sizeof(DUMP_REGS[0]); i++) {
        uint8_t v = 0;
        bool ok = es8388_read(i2c, DUMP_REGS[i].addr, &v);
        printf(" %s=%02X%s", DUMP_REGS[i].name, v, ok ? "" : "?");
    }
    printf("\n");
    fflush(stdout);
}

// Full register-space dump (0x00-0x35) for cascade debug.  Prints 8 regs per
// line with "?" suffix on I2C read failure.  Use to catch any register the
// curated DUMP_REGS list is missing.
static void dump_all_regs(i2c_inst_t *i2c, const char *label) {
    printf("  all_regs @ %s:\n", label);
    for (uint8_t base = 0x00; base <= 0x35; base += 8) {
        printf("    %02X:", base);
        for (uint8_t off = 0; off < 8 && (base + off) <= 0x35; off++) {
            uint8_t v = 0;
            bool ok = es8388_read(i2c, base + off, &v);
            printf(" %02X%s", v, ok ? " " : "?");
        }
        printf("\n");
    }
    fflush(stdout);
}

int main() {
    fpu_enable_flush_to_zero();   // Core 0's FPU (head conv + DSP chain) — flush denormals
    stdio_init_all();
    sleep_ms(300);
#if ENABLE_IR
    printf("\nTone Trixter (pico_tone_trixter)\n");
#else
    printf("\nTone Trixter — codec bench (ENABLE_IR=0, passthrough)\n");
#endif
    fflush(stdout);

    // ---- MCLK — must be running before ES8388 --------------------------------
    clock_gpio_init(MCLK_PIN, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, 12.20703125f);  // exact: 150MHz / (48kHz × 256)
    gpio_set_drive_strength(MCLK_PIN, GPIO_DRIVE_STRENGTH_2MA);
    sleep_ms(10);

    // ---- 1 kHz PWM test tone on GPIO 2 → attenuator → LIN2 ------------------
    // Wiring: GPIO2 --[100kΩ]--+--[0.1µF]-- LIN2
    //                          |
    //                        [10kΩ]
    //                          |
    //                         GND
    // 100kΩ/10kΩ divider: ~0.3 Vpp into LIN2, no startup transient.
#if ENABLE_PWM_TEST
    {
        gpio_set_function(2, GPIO_FUNC_PWM);
        uint slice = pwm_gpio_to_slice_num(2);
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv(&cfg, 8.0f);        // 150 MHz / 8 = 18.75 MHz timer
        pwm_config_set_wrap(&cfg, 18750 - 1);      // 18.75 MHz / 18750 = 1000 Hz
        pwm_init(slice, &cfg, true);
        pwm_set_gpio_level(2, 18750 / 2);          // 50% duty cycle = square wave
    }
#else
    // PWM disabled — drive GPIO 2 low so the attenuator network doesn't float
    // and couple noise into LIN2 when an external signal is being tested.
    gpio_init(2);
    gpio_set_dir(2, GPIO_OUT);
    gpio_put(2, 0);
#endif

    // Cascade-debug scope trigger — idle LOW, rises when sync-loss detected.
    gpio_init(CASCADE_TRIG_PIN);
    gpio_set_dir(CASCADE_TRIG_PIN, GPIO_OUT);
    gpio_put(CASCADE_TRIG_PIN, 0);

    // ---- Tone table for 440 Hz boot tone (50% FS amplitude) -----------------
#if BOOT_TONE_SHAPE == 1
    for (int i = 0; i < 256; i++) {
        int32_t v = (i < 128) ? (int32_t)(i * 0x01000000 - 0x40000000)
                              : (int32_t)(0x40000000 - (i - 128) * 0x01000000);
        s_sine_table[i] = v;
    }
#else
    for (int i = 0; i < 256; i++)
        s_sine_table[i] = (int32_t)(sinf(2.0f * 3.14159265f * i / 256.0f) * 0x40000000);
#endif

    // ---- Start I2S output (SCLK/LRCLK generated by PIO0 SM0) ----------------
    memset(s_out_buf, 0, sizeof(s_out_buf));
    i2s_output_init(s_out_buf[0], s_out_buf[1], passthrough_cb);

    // ---- ES8388 init: chippower → config → ADCPOWER=0x00 → sclk_resume ----
    sclk_quiesce();
    i2c_bus_recover();
    i2c_setup();
    es8388_sync_cycle(i2c1);

    printf("ES8388 init done");
    {
        uint8_t r02, r03, r08, r0C, r17;
        bool rb = es8388_read(i2c1, 0x02, &r02) && es8388_read(i2c1, 0x03, &r03)
               && es8388_read(i2c1, 0x08, &r08) && es8388_read(i2c1, 0x0C, &r0C)
               && es8388_read(i2c1, 0x17, &r17);
        if (rb) printf("Regs: CHIPPOWER=%02X ADCPOWER=%02X MASTERMODE=%02X ADCCONTROL4=%02X DACCONTROL1=%02X\n",
                       r02, r03, r08, r0C, r17);
        else    printf("Regs: readback FAILED (SCLK crosstalk)\n");
    }
    dump_regs(i2c1, "init");
    fflush(stdout);

    // ---- DOUT connectivity check -------------------------------------------
    gpio_init(ES8388_DOUT_PIN);
    gpio_set_dir(ES8388_DOUT_PIN, GPIO_IN);
    gpio_pull_up(ES8388_DOUT_PIN);   sleep_us(10);
    int pu = gpio_get(ES8388_DOUT_PIN);
    gpio_pull_down(ES8388_DOUT_PIN); sleep_us(10);
    int pd = gpio_get(ES8388_DOUT_PIN);
    gpio_disable_pulls(ES8388_DOUT_PIN);
    const char *dout_state = (pu == 0 && pd == 0) ? "DRIVEN LOW (synced, I2S active)"
                           : (pu == 1 && pd == 1) ? "DRIVEN HIGH (not synced)"
                           :                        "FLOATING — check wire/pin";
    printf("DOUT GPIO %d: pull-up=%d pull-down=%d → %s\n",
           ES8388_DOUT_PIN, pu, pd, dout_state);
    fflush(stdout);

    // ---- Input PIO (PIO1 SM0) — i2s_in_slave watches GPIO 16/17 directly ---
    s_in_pio = pio1;
    s_in_sm  = 0;
    static uint s_in_pio_offset;
    s_in_pio_offset = pio_add_program(s_in_pio, &i2s_in_slave_program);
    uint offset = s_in_pio_offset;
    pio_sm_config c = i2s_in_slave_program_get_default_config(offset);
    sm_config_set_in_pins(&c, ES8388_DOUT_PIN);
    sm_config_set_in_shift(&c, false, true, 32);   // shift left, autopush at 32
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);                // full sys_clk; wait-gpio handles timing
    pio_gpio_init(s_in_pio, ES8388_DOUT_PIN);
    pio_sm_set_consecutive_pindirs(s_in_pio, s_in_sm, ES8388_DOUT_PIN, 1, false);
    pio_sm_init(s_in_pio, s_in_sm, offset + i2s_in_slave_offset_entry_point, &c);

    // ---- Input DMA ping-pong ------------------------------------------------
    s_in_chan[0] = dma_claim_unused_channel(true);
    s_in_chan[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        dma_channel_config dc = dma_channel_get_default_config(s_in_chan[i]);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        channel_config_set_dreq(&dc, pio_get_dreq(s_in_pio, s_in_sm, false));
        channel_config_set_chain_to(&dc, s_in_chan[1 - i]);
        dma_channel_configure(s_in_chan[i], &dc,
            s_in_buf[i], &s_in_pio->rxf[s_in_sm], I2S_BLOCK_SIZE * 2, false);
        dma_channel_set_irq1_enabled(s_in_chan[i], true);
    }
    irq_set_exclusive_handler(DMA_IRQ_1, input_dma_irq1_handler);
    irq_set_enabled(DMA_IRQ_1, true);

#if ENABLE_IR
    // Initialise the dual-core convolver with the FULL NT1-A acoustic IR, launch
    // the Core 1 tail worker, then warm up. Must happen BEFORE the input PIO goes
    // live so the IRQ handler always sees a valid convolver state. Warm-up (one
    // zero pass + one sine pass) exercises every float code path before the first
    // real-audio block — same lazy-patching mitigation as audio/dsp.cpp.
    {
        // Binary handoff semaphores. `done` starts PRE-RELEASED (count 1): the
        // convolver's process() calls waitForBackgroundProcessing() BEFORE the
        // first startBackgroundProcessing() (see TwoStageFFTConvolver::process),
        // so without this initial token the first tail block would deadlock.
        sem_init(&s_sem_tail_do,   0, 1);
        sem_init(&s_sem_tail_done, 1, 1);
        // Input IRQ → foreground block handoff (starts empty; IRQ releases per block).
        sem_init(&s_sem_block_ready, 0, 1);

        if (!s_convolver.init(IR_HEAD_BLOCK, IR_TAIL_BLOCK,
                              s_ir_table[s_cur_ir].samples, s_ir_table[s_cur_ir].len)) {
            printf("TwoStageFFTConvolver init FAILED (heap exhausted?)\n");
            fflush(stdout);
            while (true) tight_loop_contents();
        }

        // Launch the Core 1 tail worker BEFORE warm-up so the cross-core handshake
        // is exercised end-to-end while warming up (not just on the first real block).
        multicore_launch_core1_with_stack(tail_core1_entry, s_core1_stack, sizeof(s_core1_stack));

        // Warm-up — zero pass primes OLA state, sine pass touches every float path.
        memset(s_dsp_in, 0, sizeof(s_dsp_in));
        s_convolver.process(s_dsp_in[0], s_dsp_out, I2S_BLOCK_SIZE);
        for (int j = 0; j < I2S_BLOCK_SIZE; j++) {
            s_dsp_in[0][j] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)j / 48000.0f);
        }
        s_convolver.process(s_dsp_in[0], s_dsp_out, I2S_BLOCK_SIZE);
        printf("TwoStageFFTConvolver ready: IR=%s %lu taps, head=%d tail=%d\n",
               s_ir_table[s_cur_ir].name, (unsigned long)s_ir_table[s_cur_ir].len,
               IR_HEAD_BLOCK, IR_TAIL_BLOCK);
        fflush(stdout);

        // Post-IR DSP chain (EQ -> Dynamics -> Output level). Output level seeds
        // at IR_OUTPUT_SCALE so boot audio is unchanged until a stage is enabled.
        dsp_chain_init((float)I2S_SAMPLE_RATE, IR_OUTPUT_SCALE);
        tuner_init((float)I2S_SAMPLE_RATE);
        app_preset_load(0);         // pre-seed preset 0 = "default": params + selects IR "none"
                                    // (convolution off). Applied on the first fg block. SD
                                    // config's boot_preset overrides below. No card ⇒ boots dry.

        // --- SD card: on-card presets / IRs override the built-ins (silent fallback) ---
        // If /tonetrix/{config,presets}.txt are present they replace the built-in list;
        // /tonetrix/ir/*.wav are scanned into the IR table. No card / no folder → the
        // built-ins above stand unchanged. Deferred to the fg loop: the boot preset's IR
        // switch (g_pending_ir) applies on the first block, like any preset change.
        if (sd_init() && f_mount(&s_fatfs, "", 1) == FR_OK) {
            ir_scan_sd();                                  // /tonetrix/ir/*.wav → IR table
            if (tt_store_load()) {
                int n = 0; const Preset *ps = tt_store_presets(&n);
                if (ps) dsp_chain_install_presets(ps, n);
                bool grset = false, gr = tt_store_gr_meter(&grset);
                if (grset) g_gr_oled = gr;                 // home GR-band default from config
                const char *bp = tt_store_boot_preset();
                int bi = (bp && bp[0]) ? dsp_chain_find_preset(bp) : 0;
                if (bi < 0) { printf("sd: boot_preset '%s' not found — using first\n", bp); bi = 0; }
                app_preset_load(bi);                       // params now + safe IR switch + the preset's own PGA
                printf("sd: on-card config loaded — %d preset%s, %d SD IR%s, boot '%s'\n",
                       n, n == 1 ? "" : "s", s_ir_count - N_EMBED_IR,
                       (s_ir_count - N_EMBED_IR) == 1 ? "" : "s", dsp_chain_preset_name(bi));
            } else {
                printf("sd: card present, no /tonetrix config — using built-in presets\n");
            }
        } else {
            printf("sd: no card / mount failed — using built-in presets\n");
        }

        footswitch_init();          // tuner / bypass footswitches (GPIO 18 / 19, pulled up)
        enc_dbg_init();             // encoder pins (GP4/3/2) — for the `enc` bring-up debug
        if (oled_init()) {          // SH1106 splash (shares the ES8388 I2C bus @ 0x3C)
            show_splash();
            printf("OLED 0x3C: splash up\n");
        } else {
            printf("OLED 0x3C: not found — check wiring / CS-DC-RES ties\n");
        }
        menu_init();                // encoder menu (splash stays until the first turn/click)
        printf("DSP chain ready — preset '%s'. Type 'help' over UART.\n", dsp_chain_preset_name(0));
        fflush(stdout);
    }
#else
    printf("IR convolution DISABLED — pure passthrough\n");
    fflush(stdout);
#endif

    pio_sm_set_enabled(s_in_pio, s_in_sm, true);
    dma_channel_start(s_in_chan[0]);

    // === REGIME CALIBRATION (es8388_pio_startup_lock_2026-04-26 task #20) ===
    // Wait for 4 input DMA blocks (~22 ms at 256 samples/block / 48 kHz), then
    // snapshot g_peak_l / g_peak_r. User power-cycles, scopes LOUT to classify
    // regime (small-clean / small-asym / big-asym), and correlates with the
    // peaks reported here. Goal: find a threshold that separates small-clean
    // from the other regimes for the closed-loop retry detector.
    uint32_t cal_start_irq = g_irq1_count;
    uint32_t cal_t_start   = time_us_32();
    while ((g_irq1_count - cal_start_irq) < 4 &&
           (time_us_32() - cal_t_start) < 100000) {
        tight_loop_contents();
    }
    printf("[cal] blocks=%lu peakL=%08lx peakR=%08lx span=%08lx raw0=%08lx raw1=%08lx\n",
           (unsigned long)(g_irq1_count - cal_start_irq),
           (uint32_t)g_peak_l, (uint32_t)g_peak_r,
           (uint32_t)(g_max_l - g_min_l),
           (uint32_t)s_staging_buf[g_staging_pub][0], (uint32_t)s_staging_buf[g_staging_pub][1]);
    fflush(stdout);

    printf("Running (quiet mode — diagnostics only on sync loss)\n");
    fflush(stdout);

    // --- UART crosstalk test: buffer last 10 seconds of stats, dump on sync loss ---
    // Register snapshot order: 0x09 MICAMP, 0x10 LADCVOL, 0x11 RADCVOL,
    //                          0x12 ALC ctrl, 0x13 ALC2, 0x15 ALC atk/dcy, 0x16 NGATE.
    static const uint8_t REG_ADDRS[7] = {0x09, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16};
    struct StatSnap {
        uint32_t irq1_delta, stale_delta;
        int32_t  pl, pr;
        uint32_t span;
        int32_t  raw[8];
        uint8_t  regs[7];
    };
    static StatSnap hist[10] = {0};
    int hist_head = 0;
    int hist_count = 0;

    uint32_t last_irq1 = 0, last_stale = 0;
    uint32_t last_diag = time_us_32();
    uint32_t last_fg_irq1 = g_irq1_count;   // dropped-block detector baseline
    uint32_t last_proc_t  = 0;              // gap-between-services timer baseline
    while (true) {
#if ENABLE_IR
        oled_flush_service();   // advance any in-flight audio-safe OLED flush (non-blocking)
        // CONTINUOUS foreground processing — never stop refilling staging. The
        // input IRQ only captures blocks (kept light so it can't delay output DMA);
        // here we convolve + run the DSP chain + clip into the non-published
        // staging buffer, then flip g_staging_pub so the output IRQ reads a full
        // block. Diagnostics run inline ~1 Hz WITHOUT pausing block processing —
        // the old "process for 1 s, then break out to diagnostics" structure left a
        // once-per-second gap that skipped a block (audible ~1 Hz tick).
        uint32_t uart_t0 = time_us_32();
        dsp_uart_poll();                           // non-blocking live-tuning over UART
        uint32_t uart_dt = time_us_32() - uart_t0;
        if (uart_dt > g_max_uart_us) g_max_uart_us = uart_dt;
        if (g_fsw_dbg) fsw_dbg_poll();             // raw footswitch wiring test (when 'fsw on')
        else           footswitch_poll();          // tuner / bypass stomp switches
        if (g_enc_dbg) enc_dbg_poll();             // encoder raw debug (when 'enc on')
        else           menu_poll();                // otherwise the encoder drives the OLED menu
        if (sem_acquire_timeout_ms(&s_sem_block_ready, 5)) {
            // g_proc_idx always points at the LATEST captured block, so if the input
            // IRQ advanced g_irq1_count by >1 since we last ran, the intervening
            // block(s) were skipped (never convolved) → an IR-tail discontinuity that
            // only ticks when there's signal. Count them.
            uint32_t cur_irq1 = g_irq1_count;
            if ((cur_irq1 - last_fg_irq1) > 1) g_dropped_blocks += (cur_irq1 - last_fg_irq1 - 1);
            last_fg_irq1 = cur_irq1;

            // Apply a pending preset IR switch at a safe point: park Core 1 (drain any
            // in-flight tail), re-init the convolver with the new IR, restore the token
            // process() expects. A brief glitch on switch is fine — it's a deliberate
            // user action. Done OUTSIDE the proc timer below.
            if (g_pending_ir >= 0) {
                int t = g_pending_ir;
                if (t == IR_NONE) {                       // select "none" → convolution off
                    dsp_chain_set_ir_enabled(false);      // (convolver keeps its last real IR loaded)
                    s_cur_ir = IR_NONE;
                } else if (t == s_conv_ir) {              // this real IR is already loaded → just enable
                    dsp_chain_set_ir_enabled(true);
                    s_cur_ir = t;
                } else {                                  // load/decode a different real IR
                    IrEntry *e = &s_ir_table[t];
                    const float *samp = e->samples;
                    uint32_t     len  = e->len;
                    if (e->is_sd) {                       // decode the WAV now (blocking SD read — a
                        uint32_t rate = 0, avail = 0;     // deliberate switch; glitches like any IR change)
                        int nsmp = wav_load_mono_f32(e->path, s_sd_ir_buf, SD_IR_MAX_TAPS, &rate, &avail);
                        if (nsmp > 0) {
                            samp = s_sd_ir_buf; len = (uint32_t)nsmp;
                            if (rate && rate != (uint32_t)I2S_SAMPLE_RATE)
                                printf("ir: WARN %s is %lu Hz (expected %d) — pitch/length will be off\n",
                                       e->name, (unsigned long)rate, I2S_SAMPLE_RATE);
                            if (avail > (uint32_t)SD_IR_MAX_TAPS)
                                printf("ir: WARN %s has %lu taps, truncated to %d\n",
                                       e->name, (unsigned long)avail, SD_IR_MAX_TAPS);
                        } else {
                            printf("ir: load '%s' failed (%s) — keeping current IR\n", e->path, wav_err_str(nsmp));
                            samp = NULL;
                        }
                    }
                    if (samp) {
                        sem_acquire_blocking(&s_sem_tail_done);
                        s_convolver.init(IR_HEAD_BLOCK, IR_TAIL_BLOCK, samp, len);
                        sem_release(&s_sem_tail_done);
                        s_conv_ir = t; s_cur_ir = t;
                        dsp_chain_set_ir_enabled(true);
                    }
                }
                g_pending_ir = -1;
            }

            uint32_t proc_t0 = time_us_32();
            if (last_proc_t) {                     // interval since the previous block service
                uint32_t gap = proc_t0 - last_proc_t;
                if (gap > g_max_gap_us) g_max_gap_us = gap;
            }
            last_proc_t = proc_t0;
            int b = g_proc_idx;
            // On entering/leaving tuner mode, repaint the OLED once: instant "TUNER"
            // on entry (before the first ~85 ms estimate), home restored on exit.
            static bool was_tuner = false;
            if (g_tuner != was_tuner) {
                if (g_tuner) { oled_clear(); oled_text(0, 0, "TUNER"); oled_flush(); g_on_home = false; }
                else         show_splash();          // leave tuner -> home (sets g_on_home)
                was_tuner = g_tuner;
            }
            // Bypass toggle (footswitch OR UART): refresh the home screen's state line so
            // it shows BYPASS/ready. Only if we're on the home screen (not mid-menu). One
            // ~180 ms flush at the stomp is fine; it's a one-off event.
            static bool was_bypass = g_dsp_bypass;
            if (g_dsp_bypass != was_bypass) {
                if (!g_tuner && g_on_home) show_splash();
                was_bypass = g_dsp_bypass;
            }
            // Live home-screen GR-meter band: repaint just its pages (5-7) ~5 fps while the
            // home screen is up and the meter is enabled ('gr off' hides it). Audio-safe —
            // the partial flush DMAs in the background. Suppressed in menus/tuner.
            static uint32_t last_gr = 0;
            if (g_on_home && g_gr_oled && !g_tuner) {
                uint32_t now = time_us_32();
                if ((uint32_t)(now - last_gr) >= 200000u) {
                    last_gr = now;
                    // Only flush (touch the I2C bus) when the band actually changed — no
                    // signal → no change → no bus activity → no crosstalk when unplugged.
                    if (draw_home_gr_band(false))
                        oled_flush_pages_async(HOME_GR_PAGE0, HOME_GR_PAGE1);
                }
            }
            if (g_tuner) {
                // Tuner mode: estimate pitch from the input and MUTE the output (silent
                // tuning, like a normal pedal tuner). IR + chain skipped so Core 0 has
                // budget for YIN; the estimate every ~85 ms briefly stalls the loop —
                // irrelevant while muted. OLED is repainted with each new estimate (the
                // flush is free here since the output is silent anyway).
                if (tuner_feed(s_dsp_in[b], I2S_BLOCK_SIZE)) { tuner_print_uart(); tuner_draw_oled(); }
                __builtin_memset(s_dsp_out, 0, sizeof(s_dsp_out));   // silent while tuning
            } else {
                if (dsp_chain_ir_enabled()) {      // IR stage on AND global bypass off
                    s_convolver.process(s_dsp_in[b], s_dsp_out, I2S_BLOCK_SIZE);
                } else {
                    // IR off / global bypass: dry captured block straight to the chain.
                    __builtin_memcpy(s_dsp_out, s_dsp_in[b], sizeof(s_dsp_out));
                }
                dsp_chain_process(s_dsp_out, I2S_BLOCK_SIZE);   // EQ -> Dynamics -> Output level
            }
            int wb = g_staging_pub ^ 1;            // fill the buffer the output IRQ is NOT reading
            uint32_t blk_opeak = 0, blk_oclip = 0; // output peak + DAC-clip count this block (for 'meter')
            for (int j = 0; j < I2S_BLOCK_SIZE; j++) {
                float f = s_dsp_out[j];            // output level applied by the chain's "out" stage
                if (f >= 1.0f || f <= -1.0f) blk_oclip++;   // hit the DAC full-scale before clamping = clip
                if (f >=  1.0f) f =  0.999999f;
                if (f <  -1.0f) f = -1.0f;
                int32_t l = (int32_t)(f * 2147483648.0f);
                uint32_t al = (uint32_t)(l < 0 ? -(int64_t)l : l);
                if (al > blk_opeak) blk_opeak = al;
                s_staging_buf[wb][j * 2]     = l;
                s_staging_buf[wb][j * 2 + 1] = l;
            }
            if (g_meter) {                         // commit peak-hold + clip count only while metering
                if (blk_opeak > g_out_peak_q) g_out_peak_q = blk_opeak;
                g_out_clip += blk_oclip;
            }
            g_staging_pub = wb;                    // atomic publish to the output IRQ
            uint32_t proc_dt = time_us_32() - proc_t0;   // includes the cross-core tail wait
            if (proc_dt > g_max_proc_us) g_max_proc_us = proc_dt;
        }
        if ((time_us_32() - last_diag) < 1000000u) continue;   // diagnostics only ~1 Hz
        last_diag += 1000000u;
#else
        sleep_ms(1000);
#endif
        uint32_t diag_t0 = time_us_32();
        uint32_t irq1  = g_irq1_count;
        uint32_t stale = g_stale_count;
        int32_t  pl    = g_peak_l;
        int32_t  pr    = g_peak_r;
        int32_t  mn    = g_min_l;
        int32_t  mx    = g_max_l;
        uint32_t span  = (uint32_t)(mx - mn);

        // Capture to ring buffer — NO printf during normal operation (UART silence).
        StatSnap *s = &hist[hist_head];
        s->irq1_delta  = irq1 - last_irq1;
        s->stale_delta = stale - last_stale;
        s->pl = pl; s->pr = pr; s->span = span;
        for (int k = 0; k < 8; k++) s->raw[k] = s_staging_buf[g_staging_pub][k];
        // Registers have been shown to never drift — skip per-second I2C polls
        // to avoid SDA/SCL edges coupling into the audio path (fridge buzz).
        // Regs are still snapshotted into the ring buffer on sync loss below.
        for (int k = 0; k < 7; k++) s->regs[k] = 0;
        hist_head = (hist_head + 1) % 10;
        if (hist_count < 10) hist_count++;

        last_irq1  = irq1;
        last_stale = stale;

#if ENABLE_IR
        // Live compressor gain-reduction meter (toggle: "meter on|off"). One short line
        // per second — costs ~1 block/s (a faint blip) like any UART print, fine while
        // dialing the comp threshold; turn off for clean playing.
        if (g_meter) {
            float in = dsp_chain_comp_in_db();   // peak level hitting the comp
            float gr = dsp_chain_comp_gr_db();    // peak gain reduction
            uint32_t opk = g_out_peak_q; g_out_peak_q = 0;   // output peak-hold: read + reset
            uint32_t ocl = g_out_clip;   g_out_clip   = 0;   // DAC-output clip count: read + reset
            uint32_t icl = g_in_clip;    g_in_clip    = 0;   // ADC-input clip count: read + reset
            float out_db = opk > 0 ? 20.0f * log10f((float)opk / 2147483648.0f) : -99.0f;
            printf("comp in %6.1f dBFS   GR %5.1f dB   out %6.1f dBFS   clip[ADC %lu DAC %lu]%s\n",
                   (double)in, (double)gr, (double)out_db,
                   (unsigned long)icl, (unsigned long)ocl,
                   (icl || ocl) ? "  *** CLIP ***" : "");
            fflush(stdout);
        }
#endif

        // Live one-line status. Default OFF (DEBUG_LIVE_PRINT) — with IR enabled the
        // blocking UART printf in this foreground thread drops an audio block and
        // causes a once-per-second blip. Enable only for bench telemetry.
#if DEBUG_LIVE_PRINT
        printf("live pkL=%08lX pkR=%08lX raw0=%08lX\n",
               (uint32_t)pl, (uint32_t)pr, (uint32_t)s_staging_buf[g_staging_pub][0]);
        fflush(stdout);
#endif

        // Sync-loss detection: span == 0 means every sample in the last block was
        // identical — i.e. the ADC is stuck on any constant (0x00000000, 0xFFFFFFFE,
        // or a rail). Guard against the legitimate boot state (no audio yet) by
        // only firing after we've seen a non-zero sample at least once.
        static bool sync_lost_printed = false;
        static bool seen_real_sample  = false;
        if (!seen_real_sample && (pl != 0 || pr != 0)) seen_real_sample = true;
        bool lost_sync = (span == 0) && seen_real_sample;

        if (lost_sync && seen_real_sample && !sync_lost_printed) {
            // Fire scope trigger BEFORE any printf — printfs take many ms and
            // would push the cascade event too far back into the pre-trigger buffer.
            gpio_put(CASCADE_TRIG_PIN, 1);
            printf("  --- history leading to SYNC LOST (static samples) ---\n");
            for (int i = 0; i < hist_count; i++) {
                int idx = (hist_head + 10 - hist_count + i) % 10;
                StatSnap *h = &hist[idx];
                printf("  t-%ds  %6lu  %6lu  pkL=%08lX pkR=%08lX span=%08lX\n",
                       hist_count - i, h->irq1_delta, h->stale_delta,
                       (uint32_t)h->pl, (uint32_t)h->pr, h->span);
                printf("    raw:");
                for (int k = 0; k < 8; k++) printf(" %08lX", (uint32_t)h->raw[k]);
                printf("\n");
            }
            printf("  SYNC LOST — no auto-resync; investigate root cause\n");
            dump_regs(i2c1, "sync_lost");
            fflush(stdout);
            sync_lost_printed = true;
        }
        uint32_t diag_dt = time_us_32() - diag_t0;
        if (diag_dt > g_max_diag_us) g_max_diag_us = diag_dt;
    }
}