#include "audio/pipeline.h"
#include "i2s/i2s.h"
#include "pico/stdlib.h"
#include "pico/sync.h"
#include <string.h>
#include <math.h>

/* ---- I2S DMA ping-pong buffers (stereo int32, interleaved L/R) ---- */
static int32_t s_tx_buf[2][I2S_BLOCK_SIZE * 2];

/* ---- Shared DSP float buffers ---- */
float        g_dsp_in[2][DSP_BLOCK_SIZE];
float        g_dsp_out[2][DSP_BLOCK_SIZE];
volatile int g_active_dsp_buf = 0;

/* ---- Display snapshots (written by Core 0 IRQ, read by main) ---- */
float g_display_in[DSP_BLOCK_SIZE];
float g_display_out[DSP_BLOCK_SIZE];

volatile uint32_t g_core1_count      = 0;
volatile uint32_t g_core1_checkpoint = 0;

semaphore_t g_sem_input_ready;
semaphore_t g_sem_output_ready;

/* ---- Sine wave emulator ---- */
/* Pre-computed 256-entry sine table (Q15, i.e. range ±32767).
 * Filled once at init; avoids calling sinf() in the IRQ. */
static int16_t  s_sine_tbl[256];
static uint32_t s_phase     = 0;
static uint32_t s_phase_inc = 0;
static float    s_gain      = 0.5f;   /* 0 dBFS / 2 — leaves headroom for reverb */

static void sine_emulator_init(uint32_t freq_hz) {
    for (int i = 0; i < 256; i++) {
        s_sine_tbl[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * i / 256.0f));
    }
    /* phase_inc in 8.24 fixed-point: (freq / sample_rate) * 2^24 * 256 */
    s_phase_inc = (uint32_t)((float)freq_hz * (float)(1u << 24) / (float)I2S_SAMPLE_RATE);
}

static void sine_emulator_fill(float *buf, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t idx = (s_phase >> 16) & 0xFF;
        buf[i]      = (float)s_sine_tbl[idx] * (s_gain / 32768.0f);
        s_phase    += s_phase_inc;
    }
}

/* ---- Format conversion: float [-1,1] → int32 stereo ---- */
static void float_to_i2s(const float *src, int32_t *dst, int n) {
    for (int i = 0; i < n; i++) {
        float f = src[i];
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        int32_t s   = (int32_t)(f * 2147483647.0f);
        dst[i * 2]     = s;   /* left  */
        dst[i * 2 + 1] = s;   /* right (mono duplicated) */
    }
}

/* ---- DMA completion callback (called from DMA_IRQ_0 on Core 0) ---- */
/*
 * Flow per callback:
 *   1. Try to grab the DSP output that Core 1 just finished → copy to I2S buffer.
 *   2. Toggle the active DSP buffer index.
 *   3. Fill the next DSP input block with sine samples.
 *   4. Signal Core 1 that new input is ready.
 */
static void i2s_dma_cb(int32_t *buf_done) {
    int fill_slot = (buf_done == s_tx_buf[0]) ? 0 : 1;

    /* Grab completed DSP output (non-blocking; output silence on overrun). */
    if (sem_try_acquire(&g_sem_output_ready)) {
        /* Snapshot before converting — this is the last good convolver output. */
        memcpy(g_display_out, g_dsp_out[g_active_dsp_buf], DSP_BLOCK_SIZE * sizeof(float));
        float_to_i2s(g_dsp_out[g_active_dsp_buf], s_tx_buf[fill_slot], I2S_BLOCK_SIZE);
        g_active_dsp_buf ^= 1;
    } else {
        /* Core 1 overran — output silence rather than stale data. */
        memset(s_tx_buf[fill_slot], 0, I2S_BLOCK_SIZE * 2 * sizeof(int32_t));
    }

    /* Fill new DSP input, snapshot it, then wake Core 1. */
    sine_emulator_fill(g_dsp_in[g_active_dsp_buf], I2S_BLOCK_SIZE);
    memcpy(g_display_in, g_dsp_in[g_active_dsp_buf], DSP_BLOCK_SIZE * sizeof(float));
    sem_release(&g_sem_input_ready);
}

/* ---- Public API ---- */
void pipeline_init(void) {
    /* Emulate I2S input: 440 Hz A4 sine through the convolver. */
    sine_emulator_init(440);

    /* Prime both I2S buffers with silence so DMA starts cleanly. */
    memset(s_tx_buf, 0, sizeof(s_tx_buf));

    /* Start I2S — DMA IRQs drive everything from here. */
    i2s_output_init(s_tx_buf[0], s_tx_buf[1], i2s_dma_cb);
}