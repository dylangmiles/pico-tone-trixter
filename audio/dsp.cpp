#include "audio/pipeline.h"
#include "FFTConvolver.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>
#include <math.h>
#include <cstdio>

#define IR_LENGTH 512   /* ~11.6 ms at 44100 Hz */

static fftconvolver::FFTConvolver s_convolver;
static float s_ir[IR_LENGTH];

/*
 * Generate a simple room-reverb impulse response in-place.
 *
 * Structure:
 *   [0]         direct sound (impulse)
 *   [22,45,...] sparse early reflections with amplitude decay
 *   [1..n-1]    diffuse tail — LCG noise × exponential envelope
 *
 * The exponential envelope is computed with repeated multiplication
 * (no transcendental functions needed at runtime).
 * Decay constant: amp *= 0.98836 per sample gives ~−60 dB over 512 samples.
 */
static void generate_reverb_ir(float *ir, int n) {
    memset(ir, 0, (size_t)n * sizeof(float));

    ir[0] = 1.0f;   /* direct sound */

    /* Early reflections */
    if (n > 22)  ir[22]  = 0.50f;
    if (n > 45)  ir[45]  = 0.35f;
    if (n > 88)  ir[88]  = 0.25f;
    if (n > 132) ir[132] = 0.18f;

    /* Diffuse tail with exponential decay */
    uint32_t seed = 0xDEADBEEFu;
    float amp = 0.40f;
    for (int i = 1; i < n; i++) {
        seed     = seed * 1664525u + 1013904223u;   /* LCG */
        float s  = (float)(int32_t)seed * (1.0f / 2147483648.0f);
        ir[i]   += s * amp;
        amp     *= 0.98836f;
    }

    /* Normalise peak to 0.9 to leave some headroom */
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = ir[i] < 0.0f ? -ir[i] : ir[i];
        if (a > peak) peak = a;
    }
    if (peak > 0.0f) {
        float scale = 0.9f / peak;
        for (int i = 0; i < n; i++) ir[i] *= scale;
    }
}

/* Core 1 entry point: wait for input blocks, run convolver, signal output ready. */
static void dsp_core1_entry(void) {
    while (true) {
        sem_acquire_blocking(&g_sem_input_ready);

        int idx = g_active_dsp_buf;
        s_convolver.process(g_dsp_in[idx], g_dsp_out[idx], DSP_BLOCK_SIZE);

        sem_release(&g_sem_output_ready);
    }
}

/*
 * Initialise the FFT convolver and launch Core 1.
 * Must be called from main() (Core 0) before pipeline_init().
 */
void dsp_init(void) {
    /* Semaphores must be ready before Core 1 starts. */
    sem_init(&g_sem_input_ready,  0, 1);
    sem_init(&g_sem_output_ready, 0, 1);
    /* Pre-prime output_ready so the first DMA callback can proceed without waiting. */
    sem_release(&g_sem_output_ready);

    generate_reverb_ir(s_ir, IR_LENGTH);

    bool ok = s_convolver.init(DSP_BLOCK_SIZE, s_ir, IR_LENGTH);
    if (!ok) {
        printf("FFTConvolver init failed (out of heap?)\n");
        while (true) tight_loop_contents();
    }

    printf("FFTConvolver ready: block=%d IR=%d samples\n", DSP_BLOCK_SIZE, IR_LENGTH);

    multicore_launch_core1(dsp_core1_entry);
}