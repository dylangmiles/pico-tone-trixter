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
#include "audio/samples/ir_array_tanglewood.h"   // active IR: Tanglewood (K&K passive). Swap to ir_array_garrison.h for the Garrison.
#include "audio/dsp_chain.h"
#include "audio/tuner.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include <cstring>
#include <cstdio>
#include <cmath>

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

#if ENABLE_IR
// Full IR via TwoStageFFTConvolver:
//   - head (64-sample blocks) runs synchronously on Core 0 → low-latency direct sound
//   - tail (512-sample blocks) runs in the background on Core 1 → late reflections
// Replaces the old single-core 512-tap truncation: now the WHOLE 2048-tap IR, with
// latency still set by the 64-sample head (~1-2 ms of DSP), not a full extra block.
// Matches the CLAUDE.md perf projection (Core 0 head ~0.6 ms, Core 1 tail ~1.5 ms).
#define IR_HEAD_BLOCK   64
#define IR_TAIL_BLOCK   512

// Seeds the DSP chain's "out" stage (final output level). Headroom is now handled
// up front by the chain's "in" trim (the IR adds ~25 dB on transients), so the
// output stage runs at unity — the K&K-slide gain staging dialed 2026-06-07:
// in.level 0.30 → comp → out.level 1.00.
#define IR_OUTPUT_SCALE 1.0f

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
static volatile bool     g_tuner = false;        // tuner mode: pitch-detect dry input, UART needle

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
                if (strcmp(line, "tuner on") == 0)       { g_tuner = true;  printf("tuner=on (dry monitor; 'tuner off' to resume)\n"); }
                else if (strcmp(line, "tuner off") == 0) { g_tuner = false; printf("tuner=off\n"); }
                else if (strcmp(line, "meter on") == 0)  { g_meter = true;  printf("meter=on\n"); }
                else if (strcmp(line, "meter off") == 0) { g_meter = false; printf("meter=off\n"); }
                else if (strcmp(line, "stats") == 0)
                    printf("blocks=%lu dropped=%lu proc=%lu tail=%lu uart=%lu diag=%lu gap=%lu us\n",
                           (unsigned long)g_irq1_count, (unsigned long)g_dropped_blocks,
                           (unsigned long)g_max_proc_us, (unsigned long)g_max_tail_us,
                           (unsigned long)g_max_uart_us, (unsigned long)g_max_diag_us,
                           (unsigned long)g_max_gap_us);
                else if (!dsp_chain_command(line))
                    printf("? '%s' (try help)\n", line);
                n = 0;
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
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
    for (int i = 0; i < 2; i++) {
        if (!dma_channel_get_irq1_status(s_in_chan[i])) continue;
        dma_channel_acknowledge_irq1(s_in_chan[i]);

        const int32_t *src = s_in_buf[i];
        int32_t peak_l = 0, peak_r = 0, min_l = 0x7fffffff, max_l = (int32_t)0x80000000;
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
            if (l_raw < min_l) min_l = l_raw;
            if (l_raw > max_l) max_l = l_raw;
        }

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
    printf("\nES8388 passthrough\n");
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

        if (!s_convolver.init(IR_HEAD_BLOCK, IR_TAIL_BLOCK, ir_samples, ir_num_samples)) {
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
        printf("TwoStageFFTConvolver ready: head=%d tail=%d IR=%lu taps (full) scale=%.2f\n",
               IR_HEAD_BLOCK, IR_TAIL_BLOCK, (unsigned long)ir_num_samples,
               (double)IR_OUTPUT_SCALE);
        fflush(stdout);

        // Post-IR DSP chain (EQ -> Dynamics -> Output level). Output level seeds
        // at IR_OUTPUT_SCALE so boot audio is unchanged until a stage is enabled.
        dsp_chain_init((float)I2S_SAMPLE_RATE, IR_OUTPUT_SCALE);
        tuner_init((float)I2S_SAMPLE_RATE);
        printf("DSP chain ready — type 'help' over UART for live tuning.\n");
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
        if (sem_acquire_timeout_ms(&s_sem_block_ready, 5)) {
            // g_proc_idx always points at the LATEST captured block, so if the input
            // IRQ advanced g_irq1_count by >1 since we last ran, the intervening
            // block(s) were skipped (never convolved) → an IR-tail discontinuity that
            // only ticks when there's signal. Count them.
            uint32_t cur_irq1 = g_irq1_count;
            if ((cur_irq1 - last_fg_irq1) > 1) g_dropped_blocks += (cur_irq1 - last_fg_irq1 - 1);
            last_fg_irq1 = cur_irq1;
            uint32_t proc_t0 = time_us_32();
            if (last_proc_t) {                     // interval since the previous block service
                uint32_t gap = proc_t0 - last_proc_t;
                if (gap > g_max_gap_us) g_max_gap_us = gap;
            }
            last_proc_t = proc_t0;
            int b = g_proc_idx;
            if (g_tuner) {
                // Tuner mode: estimate pitch from the dry input, monitor dry (skip the
                // IR + chain so Core 0 has budget for YIN). The estimate every ~85 ms
                // briefly stalls the passthrough — irrelevant while tuning.
                if (tuner_feed(s_dsp_in[b], I2S_BLOCK_SIZE)) tuner_print_uart();
                __builtin_memcpy(s_dsp_out, s_dsp_in[b], sizeof(s_dsp_out));
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
            for (int j = 0; j < I2S_BLOCK_SIZE; j++) {
                float f = s_dsp_out[j];            // output level applied by the chain's "out" stage
                if (f >=  1.0f) f =  0.999999f;
                if (f <  -1.0f) f = -1.0f;
                int32_t l = (int32_t)(f * 2147483648.0f);
                s_staging_buf[wb][j * 2]     = l;
                s_staging_buf[wb][j * 2 + 1] = l;
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
            printf("comp in %6.1f dBFS   GR %5.1f dB%s\n",
                   (double)in, (double)gr, gr <= -0.1f ? "" : "  (none)");
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