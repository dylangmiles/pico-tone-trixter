/*
 * offline_test — applies the embedded IR to the embedded piezo recording and
 * streams the processed WAV over USB CDC for capture on the host.
 *
 * Workflow:
 *   1. Flash offline_test.uf2
 *   2. Run:  python3 tools/capture_wav.py /dev/cu.usbmodem* output.wav
 *      (The script sends a newline which triggers processing)
 *   3. Open output.wav in Audacity alongside the original piezo recording
 *
 * All printf status goes to USB CDC.
 * WAV binary is written inline between WAV_DATA_START and WAV_DATA_END markers.
 *
 * Build note: this target does NOT use copy_to_ram — the 1.5 MB piezo binary
 * lives in flash and is accessed via XIP.  Only the firmware code is in SRAM.
 */

#include "pico/stdlib.h"
#include "FFTConvolver.h"

#include <cstring>
#include <cstdio>
#include <cmath>

#include "audio/samples/ir_array.h"   // ir_samples[], ir_num_samples, ir_sample_rate

// Piezo raw float32 samples embedded from audio/samples/piezo_raw.bin via objcopy.
// objcopy derives symbol names from the filename: hyphens and dots → underscores.
extern "C" {
    extern const float _binary_piezo_raw_bin_start[];
    extern const float _binary_piezo_raw_bin_end[];
}

// Block size for the convolver.  256 is efficient for a 2048-sample IR
// (8 segments × 512-point FFT).  Latency doesn't matter for offline processing.
static constexpr uint32_t BLOCK_SIZE  = 256;
static constexpr uint32_t SAMPLE_RATE = 48000;

// ---------------------------------------------------------------------------
// Raw byte output — writes to USB CDC without stdio newline translation.
// All WAV binary goes through this; printf is used only for text status.
// ---------------------------------------------------------------------------
static void write_bytes(const void* buf, uint32_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    for (uint32_t i = 0; i < len; i++) {
        putchar_raw(p[i]);
    }
}

static void write_u16_le(uint16_t v) {
    uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
    write_bytes(b, 2);
}

static void write_u32_le(uint32_t v) {
    uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
    write_bytes(b, 4);
}

// ---------------------------------------------------------------------------

int main() {
    stdio_init_all();   // UART at default 115200 via debug probe

    // Print "READY" once per second until the host sends any byte.
    // This avoids the race where the Pico prints before USB CDC is connected,
    // or where the host sends the trigger before getchar() is ready.
    // capture_wav.py waits for "READY" before sending the trigger newline.
    while (true) {
        printf("READY\n");
        fflush(stdout);
        int c = getchar_timeout_us(1000000);   // 1-second poll
        if (c != PICO_ERROR_TIMEOUT) break;
    }

    const float*    piezo   = _binary_piezo_raw_bin_start;
    const uint32_t  n_piezo = static_cast<uint32_t>(
        _binary_piezo_raw_bin_end - _binary_piezo_raw_bin_start);

    printf("IR:    %u samples @ %u Hz  (%.1f ms)\n",
           ir_num_samples, ir_sample_rate,
           1000.0f * ir_num_samples / ir_sample_rate);
    printf("Piezo: %u samples @ %u Hz  (%.2f sec)\n",
           n_piezo, SAMPLE_RATE, (float)n_piezo / SAMPLE_RATE);
    printf("Block: %u samples\n", BLOCK_SIZE);
    fflush(stdout);

    // Init convolver
    fftconvolver::FFTConvolver convolver;
    if (!convolver.init(BLOCK_SIZE, ir_samples, ir_num_samples)) {
        printf("ERROR: FFTConvolver init failed (out of heap?)\n");
        fflush(stdout);
        while (true) tight_loop_contents();
    }

    const uint32_t n_out      = n_piezo + ir_num_samples - 1;
    const uint32_t data_bytes = n_out * sizeof(int16_t);  // 16-bit PCM output

    printf("Output: %u samples (%u bytes)\n", n_out, data_bytes);
    printf("Processing...\n");
    fflush(stdout);

    // -----------------------------------------------------------------------
    // Everything from here until WAV_DATA_END is binary — no printf calls.
    // -----------------------------------------------------------------------
    printf("WAV_DATA_START\n");
    fflush(stdout);

    // Standard 44-byte PCM WAV header
    write_bytes("RIFF", 4);
    write_u32_le(36 + data_bytes);
    write_bytes("WAVE", 4);
    write_bytes("fmt ", 4);
    write_u32_le(16);                   // fmt chunk size
    write_u16_le(1);                    // audio format: PCM
    write_u16_le(1);                    // channels: mono
    write_u32_le(SAMPLE_RATE);
    write_u32_le(SAMPLE_RATE * 2);      // byte rate (16-bit mono)
    write_u16_le(2);                    // block align
    write_u16_le(16);                   // bits per sample
    write_bytes("data", 4);
    write_u32_le(data_bytes);

    // Process in BLOCK_SIZE chunks, streaming 16-bit PCM samples as we go.
    // Only two float buffers in SRAM at once; piezo data is read from XIP flash.
    static float   in_buf[BLOCK_SIZE];
    static float   out_buf[BLOCK_SIZE];
    static uint8_t pcm_buf[BLOCK_SIZE * 2];

    const uint32_t t_start_ms = to_ms_since_boot(get_absolute_time());
    uint32_t processed = 0;

    while (processed < n_out) {
        const uint32_t remaining_in = (processed < n_piezo) ? (n_piezo - processed) : 0u;
        const uint32_t copy_in      = (remaining_in < BLOCK_SIZE) ? remaining_in : BLOCK_SIZE;
        const uint32_t this_block   = ((n_out - processed) < BLOCK_SIZE)
                                      ? (n_out - processed) : BLOCK_SIZE;

        memset(in_buf, 0, BLOCK_SIZE * sizeof(float));
        if (copy_in > 0)
            memcpy(in_buf, piezo + processed, copy_in * sizeof(float));

        convolver.process(in_buf, out_buf, this_block);

        // Float → 16-bit PCM with clamp
        for (uint32_t i = 0; i < this_block; i++) {
            float s = out_buf[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            const int16_t v      = static_cast<int16_t>(s * 32767.0f);
            pcm_buf[i * 2]     = static_cast<uint8_t>(v & 0xFF);
            pcm_buf[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        }
        write_bytes(pcm_buf, this_block * 2);

        processed += this_block;
    }

    // -----------------------------------------------------------------------
    // WAV data complete — safe to printf again.
    // -----------------------------------------------------------------------
    const uint32_t elapsed_ms    = to_ms_since_boot(get_absolute_time()) - t_start_ms;
    const float    audio_dur_ms  = 1000.0f * n_piezo / SAMPLE_RATE;
    const float    realtime_pct  = (elapsed_ms > 0)
                                   ? (audio_dur_ms / elapsed_ms * 100.0f) : 0.0f;

    printf("\nWAV_DATA_END\n");
    printf("Elapsed: %u ms  audio: %.0f ms  =>  %.0f%% of real-time\n",
           elapsed_ms, audio_dur_ms, realtime_pct);
    fflush(stdout);

    while (true) tight_loop_contents();
}