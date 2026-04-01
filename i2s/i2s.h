#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* GPIO assignments — chosen to avoid UART (GPIO 0,1) and PWM audio (GPIO 2). */
#define I2S_DATA_PIN    26
#define I2S_BCLK_PIN    27   /* LRCLK = I2S_BCLK_PIN + 1 = 28 */

#define I2S_SAMPLE_RATE 48000
#define I2S_BLOCK_SIZE  256  /* mono samples per DMA block */

/* Called from DMA_IRQ_0 when a buffer finishes playing.
 * buf_done points to the buffer that just completed (safe to refill). */
typedef void (*i2s_callback_t)(int32_t *buf_done);

/* Initialise I2S output PIO (pio0, SM 0) and DMA ping-pong.
 * buf_a / buf_b must each hold I2S_BLOCK_SIZE * 2 int32_t words (interleaved stereo).
 * cb is called from DMA IRQ when each buffer completes; pass NULL to skip. */
void i2s_output_init(int32_t *buf_a, int32_t *buf_b, i2s_callback_t cb);

/* Incremented in the DMA IRQ handler — diagnostic counter. */
extern volatile uint32_t g_dma_irq_count;

#ifdef __cplusplus
}
#endif