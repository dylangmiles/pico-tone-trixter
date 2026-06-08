// audio/tuner.cpp — see tuner.h. YIN (cumulative-mean-normalized difference) pitch
// detection on a decimated window. O(maxlag * W) ~ 260k float ops per estimate
// (~0.5-1 ms) thanks to the /4 decimation, so it's affordable in tuner mode.

#include "audio/tuner.h"

#include <math.h>
#include <string.h>

#define TUNER_DECIM    4                 // input fs / 4 (48k -> 12k); guitar f0 <= ~1.5 kHz
#define TUNER_WIN      1024              // decimated samples per window (~85 ms at 12 kHz)
#define TUNER_MAXLAG   (TUNER_WIN / 2)   // lowest detectable f0 ~ 12000/512 ≈ 23 Hz
#define TUNER_MINLAG   8                 // highest detectable f0 ~ 12000/8  = 1500 Hz
#define TUNER_THRESH   0.15f             // YIN absolute threshold
#define TUNER_MIN_RMS  0.004f            // ~-48 dBFS; below this = no pitch (just noise)

static float       g_fs_dec  = 12000.0f;
static float       s_win[TUNER_WIN];
static int         s_fill    = 0;
static int         s_dec_cnt = 0;
static float       s_dec_acc = 0.0f;
static float       s_d[TUNER_MAXLAG + 1];   // YIN difference, then CMND in place
static TunerResult s_result;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

void tuner_init(float fs) {
    g_fs_dec  = fs / (float)TUNER_DECIM;
    s_fill    = 0;
    s_dec_cnt = 0;
    s_dec_acc = 0.0f;
    memset(&s_result, 0, sizeof(s_result));
    s_result.name = "-";
}

static void tuner_no_pitch(void) {
    s_result.valid   = false;
    s_result.freq_hz = 0.0f;
    s_result.cents   = 0.0f;
    s_result.name    = "-";
}

static void tuner_estimate(void) {
    const int W      = TUNER_WIN / 2;   // integration window
    const int maxlag = TUNER_MAXLAG;

    // Signal gate — don't chase the noise floor between notes.
    float energy = 0.0f;
    for (int i = 0; i < TUNER_WIN; i++) energy += s_win[i] * s_win[i];
    if (sqrtf(energy / TUNER_WIN) < TUNER_MIN_RMS) { tuner_no_pitch(); return; }

    // YIN difference function d(tau).
    s_d[0] = 1.0f;
    for (int tau = 1; tau <= maxlag; tau++) {
        float sum = 0.0f;
        for (int j = 0; j < W; j++) {
            float diff = s_win[j] - s_win[j + tau];
            sum += diff * diff;
        }
        s_d[tau] = sum;
    }

    // Cumulative mean normalized difference d'(tau).
    float running = 0.0f;
    for (int tau = 1; tau <= maxlag; tau++) {
        running += s_d[tau];
        s_d[tau] = (running > 0.0f) ? s_d[tau] * (float)tau / running : 1.0f;
    }

    // First dip below the absolute threshold (descend to its local min).
    int tau_est = -1;
    for (int tau = TUNER_MINLAG; tau < maxlag; tau++) {
        if (s_d[tau] < TUNER_THRESH) {
            while (tau + 1 < maxlag && s_d[tau + 1] < s_d[tau]) tau++;
            tau_est = tau;
            break;
        }
    }
    if (tau_est < 0) {   // fallback: global minimum, accepted only if reasonably clear
        int best = TUNER_MINLAG; float bestv = s_d[best];
        for (int tau = TUNER_MINLAG + 1; tau < maxlag; tau++)
            if (s_d[tau] < bestv) { bestv = s_d[tau]; best = tau; }
        if (bestv < 0.30f) tau_est = best;
    }
    if (tau_est < 0) { tuner_no_pitch(); return; }

    // Parabolic interpolation around the dip for sub-sample period precision.
    float betterTau = (float)tau_est;
    if (tau_est > 0 && tau_est < maxlag) {
        float s0 = s_d[tau_est - 1], s1 = s_d[tau_est], s2 = s_d[tau_est + 1];
        float denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (denom != 0.0f) betterTau = (float)tau_est + (s2 - s0) / denom;
    }

    float f0 = g_fs_dec / betterTau;

    // Map to nearest equal-tempered note + cents.
    float midi_f = 69.0f + 12.0f * log2f(f0 / 440.0f);
    int   midi   = (int)lroundf(midi_f);
    int   ni     = ((midi % 12) + 12) % 12;

    s_result.freq_hz = f0;
    s_result.cents   = 100.0f * (midi_f - (float)midi);
    s_result.midi    = midi;
    s_result.name    = NOTE_NAMES[ni];
    s_result.octave  = midi / 12 - 1;
    s_result.clarity = 1.0f - s_d[tau_est];
    s_result.valid   = true;
}

bool tuner_feed(const float *samples, int n) {
    bool ready = false;
    for (int i = 0; i < n; i++) {
        s_dec_acc += samples[i];
        if (++s_dec_cnt >= TUNER_DECIM) {
            s_win[s_fill++] = s_dec_acc / (float)TUNER_DECIM;   // box-average decimation
            s_dec_acc = 0.0f;
            s_dec_cnt = 0;
            if (s_fill >= TUNER_WIN) {
                tuner_estimate();
                s_fill = 0;
                ready  = true;
            }
        }
    }
    return ready;
}

TunerResult tuner_result(void) { return s_result; }