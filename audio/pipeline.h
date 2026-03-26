#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "pico/sem.h"
#include "i2s/i2s.h"

#define DSP_BLOCK_SIZE I2S_BLOCK_SIZE

/* Float buffers shared between Core 0 (pipeline) and Core 1 (DSP).
 * g_active_dsp_buf indicates which slot Core 1 should process next;
 * updated by Core 0 inside the DMA IRQ before releasing g_sem_input_ready. */
extern float            g_dsp_in[2][DSP_BLOCK_SIZE];
extern float            g_dsp_out[2][DSP_BLOCK_SIZE];
extern volatile int     g_active_dsp_buf;

/* Semaphores for Core 0 ↔ Core 1 handoff.
 * Initialised in dsp_init() before Core 1 is launched. */
extern semaphore_t g_sem_input_ready;   /* Core 0 posts; Core 1 waits */
extern semaphore_t g_sem_output_ready;  /* Core 1 posts; Core 0 waits */

/* Called once from main() (Core 0) after dsp_init().
 * Starts the I2S DMA; from this point DMA IRQs drive the pipeline. */
void pipeline_init(void);

#ifdef __cplusplus
}
#endif
