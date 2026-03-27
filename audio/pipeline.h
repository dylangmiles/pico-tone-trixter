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

/* Snapshot buffers written by Core 0 each time a block completes —
 * safe to read from main() without worrying about the ping-pong index. */
extern float g_display_in[DSP_BLOCK_SIZE];
extern float g_display_out[DSP_BLOCK_SIZE];

/* Incremented by Core 1 after every completed process() call.
 * If this stays 0 in the visualiser, Core 1 is stuck. */
extern volatile uint32_t g_core1_count;
/* 0=never started  1=entry  2=waiting on sem  3=inside process()  4=completed */
extern volatile uint32_t g_core1_checkpoint;

/* Called once from main() (Core 0) after dsp_init().
 * Starts the I2S DMA; from this point DMA IRQs drive the pipeline. */
void pipeline_init(void);

#ifdef __cplusplus
}
#endif
