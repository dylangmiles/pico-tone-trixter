/*
 * passthrough_test — transparent audio passthrough: PCM1808 ADC → Pico 2 → CS4344 DAC.
 *
 * Guitar → TL072 → PCM1808 → PIO1 (I2S in) → copy → PIO0 (I2S out) → CS4344 → output.
 *
 * Input DMA design — why two channels + DMA_IRQ_1:
 *   chain_to ping-pong keeps both channels running continuously, but the RP2350 DMA
 *   treats TRANS_COUNT=0 as 2^32 transfers. After the first fill, TRANS_COUNT reaches
 *   0 and the channel would run "forever" (never triggering the chain). DMA_IRQ_1 fires
 *   when each channel completes; at that point the channel is idle (chain already started
 *   the other one), so resetting write_addr + TRANS_COUNT is race-free.
 *
 * Channel polarity (discovered empirically):
 *   PCM1808 (FMT=HIGH, LJ format) outputs VINL during the LRCK=1 phase of i2s_in.pio,
 *   which lands in odd-indexed buffer slots (s_in_buf[i][2k+1]).
 *   Even slots (LRCK=0 phase) carry VINR — unused for mono guitar source.
 *
 * Latency: 2 × I2S_BLOCK_SIZE / 48000 ≈ 10.7 ms end-to-end.
 *
 * Wiring (PCM1808):
 *   PCM1808 VCC  → Pico VBUS     PCM1808 SCKI → GPIO 21
 *   PCM1808 GND  → Pico GND      PCM1808 BCK  → GPIO 2
 *   PCM1808 MD0  → GND           PCM1808 LRCK → GPIO 3
 *   PCM1808 MD1  → GND           PCM1808 DOUT → GPIO 4
 *   PCM1808 FMT  → 3.3V          PCM1808 VINL → TL072 SOUT
 *
 * WaveShare Pico Audio (CS4344) on GPIO 26/27/28.
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "i2s/i2s.h"
#include "i2s_in.pio.h"

#include <cstring>
#include <cstdio>

#define I2S_IN_BCK_PIN   2
#define I2S_IN_DATA_PIN  4
#define SCKI_PIN         21

static int32_t s_in_buf[2][I2S_BLOCK_SIZE * 2];
static int32_t s_out_buf[2][I2S_BLOCK_SIZE * 2];

// Staging buffer: IRQ1 converts completed input (VINL odd slots → stereo) here.
// Output callback copies from staging — no race possible since both are Core-0 ISRs.
static int32_t s_staging_buf[I2S_BLOCK_SIZE * 2];

static int  s_in_chan[2];
static PIO  s_in_pio;
static uint s_in_sm;

volatile uint32_t g_irq1_count    = 0;
volatile uint32_t g_stale_count   = 0;  // output fired before first input block arrived

// Per-block dropout tracking (updated in IRQ1, read in main loop).
// A "dropout" is a block where VINL peak is below DROPOUT_THRESHOLD.
// g_vinr_at_drop records the max VINR peak seen during those blocks:
//   if large → channel flip (VINL/VINR swapped in DMA buffer)
//   if near-zero → PCM1808 is genuinely silent / clock issue
#define DROPOUT_THRESHOLD 0x00100000  // ≈ −66 dBFS: any live guitar is above this
volatile uint32_t g_vinl_drops   = 0;
volatile int32_t  g_vinr_at_drop = 0;

// ---------------------------------------------------------------------------
// DMA_IRQ_1: input channel completion.
// Outputs VINL on left, VINR on right so a stereo recording can reveal a
// channel flip (VINR has audio exactly when VINL is silent).
//
// Both DMA_IRQ_0 (output callback) and DMA_IRQ_1 run on Core 0 and cannot
// preempt each other, so s_staging_buf requires no explicit locking.
// ---------------------------------------------------------------------------
static void input_dma_irq1_handler(void) {
    g_irq1_count++;
    for (int i = 0; i < 2; i++) {
        if (dma_channel_get_irq1_status(s_in_chan[i])) {
            dma_channel_acknowledge_irq1(s_in_chan[i]);

            // VINL is in odd slots (LRCK=1 phase, FMT=HIGH LJ convention).
            // VINR is in even slots (LRCK=0 phase) — normally unused.
            // Output VINL mono (both channels). Track VINR peak at drop to
            // confirm it's near-zero (hardware issue, not channel flip).
            const int32_t *src = s_in_buf[i];
            int32_t peak_vinl = 0, peak_vinr = 0;
            for (int j = 0; j < I2S_BLOCK_SIZE; j++) {
                int32_t vinl = src[j * 2 + 1];
                int32_t vinr = src[j * 2];
                s_staging_buf[j * 2]     = vinl;   // L out = VINL
                s_staging_buf[j * 2 + 1] = vinl;   // R out = VINL (mono)
                int32_t al = vinl < 0 ? -vinl : vinl;
                int32_t ar = vinr < 0 ? -vinr : vinr;
                if (al > peak_vinl) peak_vinl = al;
                if (ar > peak_vinr) peak_vinr = ar;
            }

            if (peak_vinl < DROPOUT_THRESHOLD) {
                g_vinl_drops++;
                if (peak_vinr > g_vinr_at_drop) g_vinr_at_drop = peak_vinr;
            }

            // Re-arm for when the other channel chains back to this one.
            dma_channel_set_write_addr(s_in_chan[i], s_in_buf[i], false);
            dma_channel_set_trans_count(s_in_chan[i], I2S_BLOCK_SIZE * 2, false);
        }
    }
}

// ---------------------------------------------------------------------------
// Output DMA callback (DMA_IRQ_0, called from i2s.c).
// buf_done: output buffer that just finished playing — refill from staging.
// ---------------------------------------------------------------------------
static void passthrough_cb(int32_t *buf_done) {
    int out_idx = (buf_done == s_out_buf[0]) ? 0 : 1;

    if (g_irq1_count == 0) {
        __builtin_memset(s_out_buf[out_idx], 0, I2S_BLOCK_SIZE * 2 * sizeof(int32_t));
        g_stale_count++;
        return;
    }

    __builtin_memcpy(s_out_buf[out_idx], s_staging_buf, I2S_BLOCK_SIZE * 2 * sizeof(int32_t));
}

// ---------------------------------------------------------------------------

int main() {
    stdio_init_all();
    printf("Passthrough test — PCM1808 → Pico 2 → CS4344\n");
    fflush(stdout);

    clock_gpio_init(SCKI_PIN, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, 12.207f);
    gpio_set_drive_strength(SCKI_PIN, GPIO_DRIVE_STRENGTH_12MA);  // 4mA default too weak for 12 MHz on breadboard

    // ---- Input PIO (PIO1 SM0) ----
    s_in_pio = pio1;
    s_in_sm  = 0;
    uint offset = pio_add_program(s_in_pio, &i2s_in_program);

    pio_sm_config c = i2s_in_program_get_default_config(offset);
    sm_config_set_in_pins(&c, I2S_IN_DATA_PIN);
    sm_config_set_sideset_pins(&c, I2S_IN_BCK_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    float div = (float)clock_get_hz(clk_sys) / (2.0f * I2S_SAMPLE_RATE * 64.0f);
    sm_config_set_clkdiv(&c, div);

    pio_gpio_init(s_in_pio, I2S_IN_BCK_PIN);
    pio_gpio_init(s_in_pio, I2S_IN_BCK_PIN + 1);
    pio_gpio_init(s_in_pio, I2S_IN_DATA_PIN);
    pio_sm_set_consecutive_pindirs(s_in_pio, s_in_sm, I2S_IN_BCK_PIN,  2, true);
    pio_sm_set_consecutive_pindirs(s_in_pio, s_in_sm, I2S_IN_DATA_PIN, 1, false);
    pio_sm_init(s_in_pio, s_in_sm, offset + i2s_in_offset_entry_point, &c);

    // ---- Output (must come first — i2s_output_init resets all DMA hardware) ----
    memset(s_out_buf, 0, sizeof(s_out_buf));
    i2s_output_init(s_out_buf[0], s_out_buf[1], passthrough_cb);

    // ---- Input DMA ping-pong ----
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

    printf("Running — audio should pass through transparently\n");
    fflush(stdout);

    // No timer sleep — __wfi() yields CPU between IRQs with zero timer overhead.
    // Testing whether sleep_ms(1000) timer alarm causes 1 Hz dropout.
    while (true) {
        asm volatile("wfi");
    }
}