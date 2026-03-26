#include "pico/stdlib.h"
#include <cstdio>

extern "C" {
    #include "audio/pipeline.h"
}

void dsp_init(void);   /* defined in audio/dsp.cpp */

int main() {
    stdio_init_all();
    printf("pico-tone-trixter starting\n");

    /* Init DSP convolver and launch Core 1 (must come before pipeline_init). */
    dsp_init();

    /* Start I2S DMA — drives the audio pipeline from here via DMA IRQ. */
    pipeline_init();

    printf("440 Hz sine through room reverb convolver running on I2S\n");

    while (true) {
        tight_loop_contents();
    }
}
