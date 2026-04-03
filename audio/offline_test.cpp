/*
 * offline_test — applies the embedded IR to the embedded piezo recording and
 * streams the processed WAV over UART for capture on the host.
 *
 * Workflow:
 *   1. Flash offline_test via CLion OpenOCD config (or UF2 drag-and-drop)
 *   2. Run:  python3 tools/capture_wav.py /dev/cu.usbmodem* output.wav
 *   3. Open output.wav in Audacity alongside the original piezo recording
 *
 * Convolver selection (set in CMakeLists.txt, default OFF):
 *   cmake -DOFFLINE_TEST_TWO_STAGE=ON ..   — TwoStageFFTConvolver (head=64, tail=2048)
 *   cmake -DOFFLINE_TEST_TWO_STAGE=OFF ..  — FFTConvolver (block=256)
 *
 * Build note: this target does NOT use copy_to_ram — the piezo binary
 * lives in flash and is accessed via XIP.  Only the firmware code is in SRAM.
 */

#include "pico/stdlib.h"
#include "FFTConvolver.h"
#ifdef USE_TWO_STAGE
#include "TwoStageFFTConvolver.h"
#endif

#include <cstring>
#include <cstdio>

#include OFFLINE_IR_HEADER              // ir_samples[], ir_num_samples, ir_sample_rate

// Piezo raw float32 samples embedded from audio/samples/piezo_raw_<guitar>.bin via objcopy.
// Symbol names are normalised to _binary_piezo_raw_bin_{start,end} via --redefine-sym.
extern "C" {
    extern const float _binary_piezo_raw_bin_start[];
    extern const float _binary_piezo_raw_bin_end[];
}

static constexpr uint32_t SAMPLE_RATE = 48000;

#ifdef USE_TWO_STAGE
// TwoStageFFTConvolver: head processes at low latency, tail amortises the bulk.
// process() is called at head block rate — this matches real-time DMA IRQ cadence.
static constexpr uint32_t HEAD_BLOCK  = 64;
static constexpr uint32_t TAIL_BLOCK  = 512;   // must be < irLen/2 for background to activate
static constexpr uint32_t BLOCK_SIZE  = HEAD_BLOCK;

// Subclass that times the tail (background) processing separately.
// In real-time this runs on Core 1; here we run it synchronously but measure it apart.
struct TimedTwoStage : public fftconvolver::TwoStageFFTConvolver {
    uint32_t tail_ms = 0;
    uint32_t tail_calls = 0;
protected:
    void startBackgroundProcessing() override {
        const uint32_t t0 = to_ms_since_boot(get_absolute_time());
        doBackgroundProcessing();
        tail_ms += to_ms_since_boot(get_absolute_time()) - t0;
        tail_calls++;
    }
    void waitForBackgroundProcessing() override {}  // already done in start
};
#else
// FFTConvolver: uniform block size.  256 = 8 segments for a 2048-sample IR.
static constexpr uint32_t BLOCK_SIZE  = 256;
#endif

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
#ifdef USE_TWO_STAGE
    printf("Mode:  TwoStageFFTConvolver  head=%u  tail=%u\n", HEAD_BLOCK, TAIL_BLOCK);
#else
    printf("Mode:  FFTConvolver  block=%u\n", BLOCK_SIZE);
#endif
    fflush(stdout);

    // Init convolver
#ifdef USE_TWO_STAGE
    TimedTwoStage convolver;
    if (!convolver.init(HEAD_BLOCK, TAIL_BLOCK, ir_samples, ir_num_samples)) {
        printf("ERROR: TwoStageFFTConvolver init failed (out of heap?)\n");
#else
    fftconvolver::FFTConvolver convolver;
    if (!convolver.init(BLOCK_SIZE, ir_samples, ir_num_samples)) {
        printf("ERROR: FFTConvolver init failed (out of heap?)\n");
#endif
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

    uint32_t process_ms = 0;   // accumulated convolver.process() time only
    uint32_t processed  = 0;

    while (processed < n_out) {
        const uint32_t remaining_in = (processed < n_piezo) ? (n_piezo - processed) : 0u;
        const uint32_t copy_in      = (remaining_in < BLOCK_SIZE) ? remaining_in : BLOCK_SIZE;
        const uint32_t this_block   = ((n_out - processed) < BLOCK_SIZE)
                                      ? (n_out - processed) : BLOCK_SIZE;

        memset(in_buf, 0, BLOCK_SIZE * sizeof(float));
        if (copy_in > 0)
            memcpy(in_buf, piezo + processed, copy_in * sizeof(float));

        // Time the convolver only — excludes UART write time
        const uint32_t t0 = to_ms_since_boot(get_absolute_time());
        convolver.process(in_buf, out_buf, this_block);
        process_ms += to_ms_since_boot(get_absolute_time()) - t0;

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
    const float audio_dur_ms = 1000.0f * n_piezo / SAMPLE_RATE;

    printf("\nWAV_DATA_END\n");
#ifdef USE_TWO_STAGE
    const uint32_t head_ms   = process_ms - convolver.tail_ms;
    const float head_rt_pct  = (head_ms > 0) ? (audio_dur_ms / head_ms * 100.0f) : 0.0f;
    // Tail budget: Core 1 must finish each tail call within TAIL_BLOCK samples of audio
    const float tail_budget_ms  = 1000.0f * TAIL_BLOCK / SAMPLE_RATE;
    const float tail_avg_ms     = convolver.tail_calls > 0
                                  ? (float)convolver.tail_ms / convolver.tail_calls : 0.0f;
    const float tail_rt_pct     = (tail_avg_ms > 0) ? (tail_budget_ms / tail_avg_ms * 100.0f) : 0.0f;
    printf("Head (Core 0): %u ms total  =>  %.0f%% of real-time\n", head_ms, head_rt_pct);
    printf("Tail (Core 1): %u ms total  %u calls  %.1f ms/call  budget %.1f ms  =>  %.0f%% of real-time\n",
           convolver.tail_ms, convolver.tail_calls, tail_avg_ms, tail_budget_ms, tail_rt_pct);
#else
    const float realtime_pct = (process_ms > 0) ? (audio_dur_ms / process_ms * 100.0f) : 0.0f;
    printf("DSP only: %u ms  audio: %.0f ms  =>  %.0f%% of real-time\n",
           process_ms, audio_dur_ms, realtime_pct);
#endif
    fflush(stdout);

    while (true) tight_loop_contents();
}