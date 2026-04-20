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
#include "audio/es8388.h"
#include "i2s_in_slave.pio.h"
#include <cstring>
#include <cstdio>
#include <cmath>

#define MCLK_PIN        21
#define ES8388_DOUT_PIN  5

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

// Test tone: 440 Hz sine wave plays for ~4 s on boot to verify DAC path.
static int32_t  s_sine_table[256];
static uint32_t g_tone_phase  = 0;
static uint32_t g_tone_blocks = 0;
#define TONE_DURATION_BLOCKS 752u   // ~4 s at 188 blocks/s
#define TONE_PHASE_INC       39370240u  // 440 Hz at 48 kHz: 2^32 * 440 / 48000

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

static void sclk_resume(void) {
    pio_gpio_init(pio0, I2S_BCLK_PIN);
    pio_gpio_init(pio0, I2S_BCLK_PIN + 1);
    pio_sm_restart(pio0, 0);   // reset SM to entry_point — clean LRCLK phase for ES8388 lock
    pio_sm_set_enabled(pio0, 0, true);
}

// ---------------------------------------------------------------------------
// ES8388 sync helpers (defined after sclk_resume)
// ---------------------------------------------------------------------------
// One sync cycle: chippower_cycle → config_only → ADCPOWER=0x00 (quiesced) → sclk_resume → adcpower_resync.
// SCLK must be quiesced on entry; returns with SCLK running.
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
            int32_t l = src[j * 2];      // left  (LRCLK=0 = even slots)
            int32_t r = src[j * 2 + 1];  // right (LRCLK=1 = odd slots)
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
    if (g_tone_blocks < TONE_DURATION_BLOCKS) {
        g_tone_blocks++;
        for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
            int32_t v = s_sine_table[g_tone_phase >> 24];
            g_tone_phase += TONE_PHASE_INC;
            buf_done[i * 2]     = v;
            buf_done[i * 2 + 1] = v;
        }
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
    {
        gpio_set_function(2, GPIO_FUNC_PWM);
        uint slice = pwm_gpio_to_slice_num(2);
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv(&cfg, 8.0f);        // 150 MHz / 8 = 18.75 MHz timer
        pwm_config_set_wrap(&cfg, 18750 - 1);      // 18.75 MHz / 18750 = 1000 Hz
        pwm_init(slice, &cfg, true);
        pwm_set_gpio_level(2, 18750 / 2);          // 50% duty cycle = square wave
    }

    // ---- Sine table for 440 Hz boot tone (50% FS amplitude) -----------------
    for (int i = 0; i < 256; i++)
        s_sine_table[i] = (int32_t)(sinf(2.0f * 3.14159265f * i / 256.0f) * 0x40000000);

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
        uint8_t r02, r03, r08, r0C;
        bool rb = es8388_read(i2c1, 0x02, &r02) && es8388_read(i2c1, 0x03, &r03)
               && es8388_read(i2c1, 0x08, &r08) && es8388_read(i2c1, 0x0C, &r0C);
        if (rb) printf("Regs: CHIPPOWER=%02X ADCPOWER=%02X MASTERMODE=%02X ADCCONTROL4=%02X\n",
                       r02, r03, r08, r0C);
        else    printf("Regs: readback FAILED (SCLK crosstalk)\n");
    }
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

    printf("Running. irq1/s  stale/s  peak_L       peak_R       span_L\n");
    fflush(stdout);

    uint32_t last_irq1 = 0, last_stale = 0;
    int resync_count = 0;
    while (true) {
        sleep_ms(1000);
        uint32_t irq1  = g_irq1_count;
        uint32_t stale = g_stale_count;
        int32_t  pl    = g_peak_l;
        int32_t  pr    = g_peak_r;
        int32_t  mn    = g_min_l;
        int32_t  mx    = g_max_l;
        uint32_t span  = (uint32_t)(mx - mn);
        printf("  %6lu  %6lu  pkL=%08lX pkR=%08lX span=%08lX\n",
               irq1 - last_irq1, stale - last_stale, (uint32_t)pl, (uint32_t)pr, span);
        printf("  raw:");
        for (int k = 0; k < 8; k++) printf(" %08lX", (uint32_t)s_staging_buf[k]);
        printf("\n");
        fflush(stdout);
        last_irq1  = irq1;
        last_stale = stale;

        // Auto-resync: all samples FFFFFFFF = DOUT stuck HIGH (ES8388 lost I2S sync).
        bool lost_sync = (mn == (int32_t)0xFFFFFFFF && mx == (int32_t)0xFFFFFFFF);
        if (lost_sync) {
            resync_count++;
            printf("  SYNC LOST (#%d) — attempting resync\n", resync_count);
            fflush(stdout);

            pio_sm_set_enabled(s_in_pio, s_in_sm, false);
            sclk_quiesce();
            i2c_bus_recover();
            i2c_setup();
            es8388_sync_cycle(i2c1);

            // Return DOUT pin to PIO1, then restart input SM cleanly at entry_point.
            pio_gpio_init(s_in_pio, ES8388_DOUT_PIN);
            pio_sm_set_consecutive_pindirs(s_in_pio, s_in_sm, ES8388_DOUT_PIN, 1, false);
            pio_sm_clear_fifos(s_in_pio, s_in_sm);
            pio_sm_restart(s_in_pio, s_in_sm);
            pio_sm_exec(s_in_pio, s_in_sm,
                        pio_encode_jmp(s_in_pio_offset + i2s_in_slave_offset_entry_point));
            pio_sm_set_enabled(s_in_pio, s_in_sm, true);
        }
    }
}