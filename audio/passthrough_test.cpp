/*
 * passthrough_test — transparent audio passthrough: PCM1808 ADC → Pico 2 → CS4344 DAC.
 *
 * Validates the complete real-time signal chain with no DSP processing.
 * Guitar → TL072 → PCM1808 → PIO1 (I2S in) → copy → PIO0 (I2S out) → CS4344 → output.
 *
 * Architecture mirrors main pipeline.cpp:
 *   - Output DMA ping-pong (PIO0, i2s.c) fires DMA_IRQ_0 each block.
 *   - Input DMA ping-pong (PIO1, i2s_in.pio) fills in_buf[0/1] continuously.
 *   - DMA IRQ callback copies the just-filled input buffer to the just-played output buffer.
 *   - Input DMA channels are re-armed in the callback (same pattern as i2s.c for output).
 *
 * Latency: 2 × I2S_BLOCK_SIZE samples = 2 × 256/48000 ≈ 10.7 ms end-to-end.
 *
 * Wiring (PCM1808 — same as pcm1808_test):
 *   PCM1808 VCC  → Pico VBUS     PCM1808 SCKI → GPIO 21
 *   PCM1808 GND  → Pico GND      PCM1808 BCK  → GPIO 2
 *   PCM1808 MD0  → GND           PCM1808 LRCK → GPIO 3
 *   PCM1808 MD1  → GND           PCM1808 DOUT → GPIO 4
 *   PCM1808 FMT  → 3.3V          PCM1808 VINL → TL072 SOUT
 *
 * WaveShare Pico Audio (CS4344) plugs onto Pico GPIO 26/27/28 as normal.
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "i2s/i2s.h"
#include "i2s_in.pio.h"

#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// GPIO assignments (PCM1808)
// ---------------------------------------------------------------------------
#define I2S_IN_BCK_PIN   2    // BCK out; LRCK = GPIO 3
#define I2S_IN_DATA_PIN  4    // DOUT in
#define SCKI_PIN         21   // 12.288 MHz system clock to PCM1808

// ---------------------------------------------------------------------------
// Buffers
// Two input buffers filled alternately by PCM1808 DMA.
// Two output buffers played alternately by CS4344 DMA (via i2s.c).
// Each buffer: I2S_BLOCK_SIZE stereo int32 words = I2S_BLOCK_SIZE * 2 * 4 bytes = 2 KB.
// ---------------------------------------------------------------------------
static int32_t s_in_buf[2][I2S_BLOCK_SIZE * 2];
static int32_t s_out_buf[2][I2S_BLOCK_SIZE * 2];

// Input DMA channels (ping-pong, mirror of i2s.c output channels)
static int s_in_chan[2];
static PIO  s_in_pio;
static uint s_in_sm;

// ---------------------------------------------------------------------------
// DMA IRQ callback — called from DMA_IRQ_0 when an output buffer completes.
// buf_done: the output buffer that just finished playing — safe to refill.
// By design, the corresponding input buffer has also just been filled.
// ---------------------------------------------------------------------------
static void passthrough_cb(int32_t *buf_done) {
    int idx = (buf_done == s_out_buf[0]) ? 0 : 1;

    // Copy PCM1808 left channel to both L and R of the output buffer.
    // Input buf[idx] just finished filling at the same time output buf[idx]
    // finished playing — they run from the same clock at the same block size.
    // PCM1808 word format (i2s_in.pio, ISR shift-left):
    //   in_buf[idx][2i]   = left  word: bit[31..8] = 24-bit audio, bit[7..0] = 0
    //   in_buf[idx][2i+1] = right word (not used — mono source)
    // CS4344 via i2s_out.pio expects int32 stereo, audio in upper bits. Direct copy works.
    const int32_t *src = s_in_buf[idx];
    int32_t       *dst = s_out_buf[idx];
    for (int i = 0; i < I2S_BLOCK_SIZE; i++) {
        int32_t s   = src[i * 2];       // left channel word from PCM1808
        dst[i * 2]     = s;             // left out
        dst[i * 2 + 1] = s;             // right out (mono duplicated)
    }

    // Re-arm this input channel so it's ready when the input DMA chain
    // cycles back to it (after the other channel finishes).
    dma_channel_set_write_addr(s_in_chan[idx], s_in_buf[idx], false);
    dma_channel_set_trans_count(s_in_chan[idx], I2S_BLOCK_SIZE * 2, false);
}

// ---------------------------------------------------------------------------

int main() {
    stdio_init_all();
    printf("Passthrough test — PCM1808 → Pico 2 → CS4344\n");
    fflush(stdout);

    // ---- SCKI: 12.288 MHz to PCM1808 via GPOUT0 ----
    clock_gpio_init(SCKI_PIN, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, 12.207f);

    // ---- Input PIO (PIO1 SM0) ----
    s_in_pio = pio1;
    s_in_sm  = 0;
    uint offset = pio_add_program(s_in_pio, &i2s_in_program);

    pio_sm_config c = i2s_in_program_get_default_config(offset);
    sm_config_set_in_pins(&c, I2S_IN_DATA_PIN);
    sm_config_set_sideset_pins(&c, I2S_IN_BCK_PIN);
    sm_config_set_in_shift(&c, false, true, 32);      // shift left, autopush 32
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    float div = (float)clock_get_hz(clk_sys) / (2.0f * I2S_SAMPLE_RATE * 64.0f);
    sm_config_set_clkdiv(&c, div);

    pio_gpio_init(s_in_pio, I2S_IN_BCK_PIN);
    pio_gpio_init(s_in_pio, I2S_IN_BCK_PIN + 1);
    pio_gpio_init(s_in_pio, I2S_IN_DATA_PIN);
    pio_sm_set_consecutive_pindirs(s_in_pio, s_in_sm, I2S_IN_BCK_PIN,  2, true);
    pio_sm_set_consecutive_pindirs(s_in_pio, s_in_sm, I2S_IN_DATA_PIN, 1, false);
    pio_sm_init(s_in_pio, s_in_sm, offset + i2s_in_offset_entry_point, &c);

    // ---- Output DMA + PIO (PIO0 SM0) via i2s.c ----
    // Must come before input DMA setup: i2s_output_init() resets all DMA hardware
    // (RESETS_RESET_DMA_BITS), which would wipe any input channels configured first.
    memset(s_out_buf, 0, sizeof(s_out_buf));
    i2s_output_init(s_out_buf[0], s_out_buf[1], passthrough_cb);

    // ---- Input DMA ping-pong (channels 0 and 1 of s_in_chan) ----
    s_in_chan[0] = dma_claim_unused_channel(true);
    s_in_chan[1] = dma_claim_unused_channel(true);

    for (int i = 0; i < 2; i++) {
        int other = 1 - i;
        dma_channel_config dc = dma_channel_get_default_config(s_in_chan[i]);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        channel_config_set_dreq(&dc, pio_get_dreq(s_in_pio, s_in_sm, false));
        channel_config_set_chain_to(&dc, s_in_chan[other]);
        dma_channel_configure(s_in_chan[i], &dc,
            s_in_buf[i], &s_in_pio->rxf[s_in_sm], I2S_BLOCK_SIZE * 2, false);
    }

    // ---- Start input simultaneously with output ----
    pio_sm_set_enabled(s_in_pio, s_in_sm, true);
    dma_channel_start(s_in_chan[0]);

    printf("Running — audio should pass through transparently\n");
    fflush(stdout);

    // Heartbeat: print DMA IRQ count and peak input level every second.
    // If peak stays 0, the PCM1808 is not producing data — check wiring.
    while (true) {
        sleep_ms(1000);
        // Sample peak from current input buffer
        int32_t peak = 0;
        for (int i = 0; i < I2S_BLOCK_SIZE * 2; i++) {
            int32_t v = s_in_buf[0][i] < 0 ? -s_in_buf[0][i] : s_in_buf[0][i];
            if (v > peak) peak = v;
        }
        printf("dma_irq=%lu  input_peak=0x%08lX\n",
               (unsigned long)g_dma_irq_count, (unsigned long)peak);
        fflush(stdout);
    }
}