#include "audio/pipeline.h"
#include "FFTConvolver.h"
#include "AudioFFT.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>
#include <math.h>
#include <cstdio>

#define IR_LENGTH 512   /* ~10.7 ms at 48000 Hz — placeholder; will use 2048-sample NT1-A IR */

static fftconvolver::FFTConvolver s_convolver;
static float s_ir[IR_LENGTH];

/* 32 KB stack for Core 1 — the Ooura FFT for a 1024-point complex transform
 * (blockSize=256, IR=512 → next power-of-2 = 1024) needs ~20-30 KB of stack. */
static uint32_t s_core1_stack[32768 / sizeof(uint32_t)];

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
    g_core1_checkpoint = 1;   /* Core 1 entered */

    while (true) {
        g_core1_checkpoint = 2;   /* waiting on semaphore */
        sem_acquire_blocking(&g_sem_input_ready);

        g_core1_checkpoint = 10;  /* about to read idx */
        int idx = g_active_dsp_buf;
        g_core1_checkpoint = 11;  /* calling process() */
        s_convolver.process(g_dsp_in[idx], g_dsp_out[idx], DSP_BLOCK_SIZE);
        g_core1_checkpoint = 12;  /* process() returned */

        g_core1_checkpoint = 4;   /* completed */
        g_core1_count++;
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

    /*
     * Warm-up: call process() twice on Core 0 — once with zeros, once with
     * a non-zero sine burst.
     *
     * The pico_float ROM shim uses lazy patching (float_table_shim_on_use_helper):
     * sf_table entries are only patched to the real ROM function on their FIRST
     * call.  A zero-input process() short-circuits many fmul paths (0×x = 0)
     * so those entries never get patched.  When Core 1 later calls process()
     * with real (non-zero) audio data it would hit an unpatched entry on ROM v1
     * silicon.  Running a non-zero pass on Core 0 first ensures every float
     * code path in the butterfly is exercised and all sf_table entries patched
     * before Core 1 starts.
     */
    static float test_in[DSP_BLOCK_SIZE];
    static float test_out[DSP_BLOCK_SIZE];

    memset(test_in, 0, sizeof(test_in));
    s_convolver.process(test_in, test_out, DSP_BLOCK_SIZE);   /* zeros — primes OLA state */

    /* Fill with a non-zero sine burst to exercise every fmul code path. */
    for (int i = 0; i < DSP_BLOCK_SIZE; i++) {
        test_in[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
    }
    printf("Core0: warm-up process() with sine input...\n");
    s_convolver.process(test_in, test_out, DSP_BLOCK_SIZE);
    printf("Core0: warm-up done, out[0]=%.4f\n", (double)test_out[0]);

    multicore_launch_core1_with_stack(dsp_core1_entry, s_core1_stack, sizeof(s_core1_stack));
}