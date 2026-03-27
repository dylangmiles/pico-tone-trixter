#include "i2s.h"
#include "i2s_out.pio.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/resets.h"

static int s_chan_a, s_chan_b;
static int32_t *s_buf[2];
static i2s_callback_t s_callback;

volatile uint32_t g_dma_irq_count = 0;

static void dma_irq0_handler(void) {
    g_dma_irq_count++;
    bool a = dma_channel_get_irq0_status(s_chan_a);
    bool b = dma_channel_get_irq0_status(s_chan_b);

    if (a) {
        dma_channel_acknowledge_irq0(s_chan_a);
        /* Re-arm chan_a so chan_b can chain back to it after it finishes. */
        dma_channel_set_read_addr(s_chan_a, s_buf[0], false);
        dma_channel_set_trans_count(s_chan_a, I2S_BLOCK_SIZE * 2, false);
        if (s_callback) s_callback(s_buf[0]);
    }
    if (b) {
        dma_channel_acknowledge_irq0(s_chan_b);
        dma_channel_set_read_addr(s_chan_b, s_buf[1], false);
        dma_channel_set_trans_count(s_chan_b, I2S_BLOCK_SIZE * 2, false);
        if (s_callback) s_callback(s_buf[1]);
    }
}

void i2s_output_init(int32_t *buf_a, int32_t *buf_b, i2s_callback_t cb) {
    s_buf[0]  = buf_a;
    s_buf[1]  = buf_b;
    s_callback = cb;

    /* Force-reset DMA and PIO0 to guarantee a clean hardware state after any
     * kind of reboot (watchdog, UF2 flash-drag, etc.).  The bootrom USB stack
     * uses DMA; without this, the DMA BUSY/IRQ bits can be stale after a
     * flash-triggered reset, preventing the first DMA completion IRQ from
     * ever firing. */
    reset_block(RESETS_RESET_DMA_BITS | RESETS_RESET_PIO0_BITS);
    unreset_block_wait(RESETS_RESET_DMA_BITS | RESETS_RESET_PIO0_BITS);

    /* ---- PIO setup ---- */
    PIO  pio    = pio0;
    uint sm     = 0;
    uint offset = pio_add_program(pio, &i2s_out_program);

    pio_sm_config c = i2s_out_program_get_default_config(offset);

    sm_config_set_out_pins(&c, I2S_DATA_PIN, 1);
    sm_config_set_sideset_pins(&c, I2S_BCLK_PIN);

    /* Shift left, autopull at 32 bits. Join FIFOs for 8-word TX depth. */
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    /* PIO clock = 2 × BCLK = 2 × (sample_rate × 64). */
    float div = (float)clock_get_hz(clk_sys) / (2.0f * I2S_SAMPLE_RATE * 64.0f);
    sm_config_set_clkdiv(&c, div);

    pio_gpio_init(pio, I2S_DATA_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN);
    pio_gpio_init(pio, I2S_BCLK_PIN + 1);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_DATA_PIN,  1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, I2S_BCLK_PIN,  2, true);

    pio_sm_init(pio, sm, offset + i2s_out_offset_entry_point, &c);

    /* ---- DMA ping-pong: chan_a ↔ chan_b chain ---- */
    s_chan_a = dma_claim_unused_channel(true);
    s_chan_b = dma_claim_unused_channel(true);

    dma_channel_config da = dma_channel_get_default_config(s_chan_a);
    channel_config_set_transfer_data_size(&da, DMA_SIZE_32);
    channel_config_set_read_increment(&da, true);
    channel_config_set_write_increment(&da, false);
    channel_config_set_dreq(&da, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&da, s_chan_b);
    dma_channel_configure(s_chan_a, &da, &pio->txf[sm], buf_a, I2S_BLOCK_SIZE * 2, false);
    dma_channel_set_irq0_enabled(s_chan_a, true);

    dma_channel_config db = dma_channel_get_default_config(s_chan_b);
    channel_config_set_transfer_data_size(&db, DMA_SIZE_32);
    channel_config_set_read_increment(&db, true);
    channel_config_set_write_increment(&db, false);
    channel_config_set_dreq(&db, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&db, s_chan_a);
    dma_channel_configure(s_chan_b, &db, &pio->txf[sm], buf_b, I2S_BLOCK_SIZE * 2, false);
    dma_channel_set_irq0_enabled(s_chan_b, true);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    pio_sm_set_enabled(pio, sm, true);
    dma_channel_start(s_chan_a);
}