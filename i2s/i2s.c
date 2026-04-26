#include "i2s.h"
#include "i2s_out.pio.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/resets.h"
#include <stdio.h>

static int s_chan_a, s_chan_b;
static int32_t *s_buf[2];
static i2s_callback_t s_callback;
static unsigned int s_pio_offset;

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

unsigned int i2s_pio_offset(void) {
    return s_pio_offset;
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
    s_pio_offset = offset;

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

    /* Init SM at program start (offset, NOT entry_point). After SM enable
     * we'll inject a JMP to entry_point via pio_sm_exec. This matches
     * elehobica/pico_audio_i2s_32b's init sequence — it avoids ambiguity
     * about which side-set state is applied at the moment SM is enabled. */
    pio_sm_init(pio, sm, offset, &c);

    /* Runtime register dump — debug 4-BCLK rotation (2026-04-25). Prints what
     * the SDK actually programmed; compare against expected fields from
     * i2s_out.pio analysis. Decoded in docs/debugging/es8388_i2s_format_2026-04-25. */
    printf("[i2s] entry=%u wrap=%u..%u  SHIFTCTRL=0x%08lx EXECCTRL=0x%08lx PINCTRL=0x%08lx\n",
           offset + i2s_out_offset_entry_point,
           offset + i2s_out_wrap_target,
           offset + i2s_out_wrap,
           (unsigned long)pio->sm[sm].shiftctrl,
           (unsigned long)pio->sm[sm].execctrl,
           (unsigned long)pio->sm[sm].pinctrl);

    /* Pre-clear the data, BCLK and LRCK pins to 0 before SM enable, matching
     * pico-extras audio_i2s_program_init. Avoids any startup glitch from
     * undefined pin state at the moment the SM begins driving them. */
    pio_sm_set_pins(pio, sm, 0);

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

    /* Enable SM before starting DMA, matching elehobica/pico_audio_i2s_32b
     * (a working 32-bit pico-extras-derived I2S driver for Pico 2). The SM
     * will stall on its first OUT (autopull on empty FIFO); dma_channel_start
     * then unblocks it cleanly with the first DMA word.
     *
     * Earlier (DMA-first) tried to pre-fill the FIFO before SM enable, but
     * that produced a fixed +6 BCLK rotation between LRCK and DIN data
     * (captures 22-25 in es8388_i2s_format_2026-04-25). The likely cause is
     * an RP2350-specific quirk where FIFO writes arriving before SM enable
     * cause the first autopull to load a partial/stale word. */
    pio_sm_drain_tx_fifo(pio, sm);
    pio_sm_set_enabled(pio, sm, true);
    /* Inject JMP to entry_point after SM is running. SM is stalled on its
     * first autopull (FIFO is empty); the injected JMP redirects PC to
     * entry_point so when DMA fills the FIFO, the SM resumes from the
     * correct loop-counter init instruction. */
    pio_sm_exec(pio, sm, pio_encode_jmp(offset + i2s_out_offset_entry_point));
    dma_channel_start(s_chan_a);
}