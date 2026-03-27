#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" {
    #include "audio/pipeline.h"
    #include "i2s/i2s.h"
}

void dsp_init(void);

/* ---- Terminal oscilloscope ---- */
#define VIS_W  64   /* columns = samples displayed per waveform */
#define VIS_H  12   /* rows    = amplitude resolution           */

static void print_waveform(const char *label, const float *buf, int len) {
    char grid[VIS_H][VIS_W + 1];
    for (int r = 0; r < VIS_H; r++) {
        memset(grid[r], ' ', VIS_W);
        grid[r][VIS_W] = '\0';
    }

    /* Draw centre line */
    int mid = VIS_H / 2;
    for (int c = 0; c < VIS_W; c++) grid[mid][c] = '-';

    /* Plot one '*' per column at the row matching the sample amplitude */
    float peak = 0.0f;
    int   step = len / VIS_W;
    for (int c = 0; c < VIS_W; c++) {
        float v = buf[c * step];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        float a = v < 0.0f ? -v : v;
        if (a > peak) peak = a;
        /* row 0 = top (+1.0), row VIS_H-1 = bottom (-1.0) */
        int row = (int)((1.0f - v) * 0.5f * (VIS_H - 1) + 0.5f);
        if (row < 0)      row = 0;
        if (row >= VIS_H) row = VIS_H - 1;
        grid[row][c] = '*';
    }

    printf("  %s  (peak %.3f)\n", label, (double)peak);
    printf("  +");
    for (int c = 0; c < VIS_W; c++) putchar('-');
    printf("+\n");

    for (int r = 0; r < VIS_H; r++) {
        const char *axis = "     ";
        if (r == 0)   axis = " +1.0";
        if (r == mid) axis = "  0.0";
        if (r == VIS_H - 1) axis = " -1.0";
        printf("%s|%s|\n", axis, grid[r]);
    }

    printf("  +");
    for (int c = 0; c < VIS_W; c++) putchar('-');
    printf("+\n");
}

int main() {
    stdio_init_all();
    printf("pico-tone-trixter starting\n");

    dsp_init();
    pipeline_init();

    static float snap_in[DSP_BLOCK_SIZE];
    static float snap_out[DSP_BLOCK_SIZE];
    uint32_t frame = 0;

    while (true) {
        sleep_ms(400);
        printf("frame=%lu  blocks=%lu  cp=%lu  dma_irq=%lu\n",
               (unsigned long)frame++,
               (unsigned long)g_core1_count,
               (unsigned long)g_core1_checkpoint,
               (unsigned long)g_dma_irq_count);
    }
}