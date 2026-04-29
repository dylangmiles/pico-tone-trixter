/*
 * es8388_test — ES8388 init + transparent audio passthrough.
 *
 * Guitar → TL072 → ES8388 LIN2 → PIO1 (i2s_in_slave) → copy → PIO0 (i2s_out) → ES8388 DAC → jack.
 *
 * ES8388 is I2S slave; Pico drives MCLK (GPIO 21), SCLK (GPIO 27), LRCLK (GPIO 28).
 * PIO1 reads DOUT (GPIO 5) by watching SCLK/LRCLK with wait-gpio — no PIO clock config needed.
 *
 * Init is two-phase to eliminate I2C/SCLK crosstalk:
 *   Phase 1 (SCLK quiesced): GPIO 27/28 driven LOW via SIO, config registers written with zero
 *                             interference on SDA/SCL.
 *   Phase 2 (SCLK running):  PIO0 resumed, es8388_adcpower_resync() triggers I2S sync via the
 *                             ADCPOWER 0xFF→0x00 transition while SCLK is live.
 *
 * Channel layout (ES8388 Left-Justified, LRCLK=0 = left):
 *   buf[j*2 + 0] = left  (guitar, LIN2)
 *   buf[j*2 + 1] = right (unused)
 *
 * Wiring:
 *   ES8388 DVDD  → 3.3V           ES8388 MCLK  → GPIO 21 (100Ω series)
 *   ES8388 AVDD  → 3.3V           ES8388 SCLK  → GPIO 27
 *   ES8388 GND   → GND            ES8388 LRCLK → GPIO 28
 *   ES8388 SDA   → GPIO 6         ES8388 DIN   → GPIO 26
 *   ES8388 SCL   → GPIO 7         ES8388 DOUT  → GPIO 5
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
#include <cstring>
#include <cstdio>
#include <cmath>

#define MCLK_PIN        21
#define ES8388_DOUT_PIN  5

// Cascade-debug scope trigger: goes HIGH the instant sync-loss is detected so
// the scope can single-trigger with pre-trigger buffer showing pre-cascade state.
#define CASCADE_TRIG_PIN 15

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

// ---- Buffers ---------------------------------------------------------------
static int32_t s_in_buf[2][I2S_BLOCK_SIZE * 2];
static int32_t s_out_buf[2][I2S_BLOCK_SIZE * 2];
static int32_t s_staging_buf[I2S_BLOCK_SIZE * 2];

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
// quiesce: stop PIO0 SM0 and drive GPIO 27 (SCLK) and GPIO 28 (LRCLK) LOW
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
        for (int j = 0; j < I2S_BLOCK_SIZE; j++) {
            // << 1 is sign-recovery, not amplitude. PIO empirically captures a
            // leading "delay zero" in bit 31 with audio bits 31..1 in bits 30..0;
            // shift-left puts the original sign bit back into bit 31. Without
            // this shift, negative samples read as huge positive values
            // (rectification → 1.66 V pk-pk sawtooth at idle, 2026-04-27).
            int32_t l = src[j * 2]     << 1;  // left  (LRCLK=0 = even slots)
            int32_t r = src[j * 2 + 1] << 1;  // right (LRCLK=1 = odd slots)
            s_staging_buf[j * 2]     = l;
            s_staging_buf[j * 2 + 1] = l;
            int32_t al = l < 0 ? -l : l;
            int32_t ar = r < 0 ? -r : r;
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
            if (l < min_l) min_l = l;
            if (l > max_l) max_l = l;
        }
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
    __builtin_memcpy(s_out_buf[idx], s_staging_buf, I2S_BLOCK_SIZE * 2 * sizeof(int32_t));
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

    // ---- Input PIO (PIO1 SM0) — i2s_in_slave watches GPIO 27/28 directly ---
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
           (uint32_t)s_staging_buf[0], (uint32_t)s_staging_buf[1]);
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
    while (true) {
        sleep_ms(1000);
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
        for (int k = 0; k < 8; k++) s->raw[k] = s_staging_buf[k];
        // Registers have been shown to never drift — skip per-second I2C polls
        // to avoid SDA/SCL edges coupling into the audio path (fridge buzz).
        // Regs are still snapshotted into the ring buffer on sync loss below.
        for (int k = 0; k < 7; k++) s->regs[k] = 0;
        hist_head = (hist_head + 1) % 10;
        if (hist_count < 10) hist_count++;

        last_irq1  = irq1;
        last_stale = stale;

        // Live one-line status — pure in-memory reads, no I2C, no audio coupling.
        printf("live pkL=%08lX pkR=%08lX raw0=%08lX\n",
               (uint32_t)pl, (uint32_t)pr, (uint32_t)s_staging_buf[0]);
        fflush(stdout);

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
    }
}