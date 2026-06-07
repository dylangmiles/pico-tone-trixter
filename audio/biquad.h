// audio/biquad.h — minimal RBJ-cookbook biquad for the Tone Trixter DSP chain.
//
// Float, Transposed Direct Form II (good float numerical behaviour, tight inner
// loop). Coefficients are computed once when a parameter changes (NOT per sample)
// from the sample rate; the per-sample real-time call is biquad_process(). Cheap
// enough (~5 mul + 4 add) to run several in series on Core 0 alongside the IR.
//
// Provided: high-pass (future rumble HPF), peaking + low/high shelf (the 3-band
// EQ). All coefficient builders reset state, so retune-while-playing is safe.
#ifndef TT_BIQUAD_H
#define TT_BIQUAD_H

#include <math.h>

#ifndef TT_BIQUAD_PI
#define TT_BIQUAD_PI 3.14159265358979f
#endif

typedef struct {
    float b0, b1, b2, a1, a2;   // feed-forward / feed-back, normalised (a0 == 1)
    float z1, z2;               // state (Transposed DF-II)
} Biquad;

static inline void biquad_reset(Biquad *bq) {
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

// Process one sample. Transposed Direct Form II.
static inline float biquad_process(Biquad *bq, float x) {
    float y = bq->b0 * x + bq->z1;
    bq->z1  = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2  = bq->b2 * x - bq->a2 * y;
    return y;
}

// RBJ high-pass. fs, f0 in Hz; q dimensionless (0.707 ≈ Butterworth). 12 dB/oct.
static inline void biquad_highpass(Biquad *bq, float fs, float f0, float q) {
    float w0    = 2.0f * TT_BIQUAD_PI * f0 / fs;
    float cw    = cosf(w0);
    float sw    = sinf(w0);
    float alpha = sw / (2.0f * q);
    float a0    = 1.0f + alpha;
    bq->b0 =  (1.0f + cw) * 0.5f / a0;
    bq->b1 = -(1.0f + cw)        / a0;
    bq->b2 =  (1.0f + cw) * 0.5f / a0;
    bq->a1 =  (-2.0f * cw)       / a0;
    bq->a2 =  (1.0f - alpha)     / a0;
    biquad_reset(bq);
}

// RBJ peaking EQ — boost/cut a band around f0. gain_db > 0 boosts, < 0 cuts.
// q sets bandwidth (higher q = narrower).
static inline void biquad_peaking(Biquad *bq, float fs, float f0, float q, float gain_db) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * TT_BIQUAD_PI * f0 / fs;
    float cw    = cosf(w0);
    float sw    = sinf(w0);
    float alpha = sw / (2.0f * q);
    float a0    = 1.0f + alpha / A;
    bq->b0 = (1.0f + alpha * A) / a0;
    bq->b1 = (-2.0f * cw)       / a0;
    bq->b2 = (1.0f - alpha * A) / a0;
    bq->a1 = (-2.0f * cw)       / a0;
    bq->a2 = (1.0f - alpha / A) / a0;
    biquad_reset(bq);
}

// RBJ low shelf — boost/cut everything below ~f0. q ≈ 0.707 for a clean shelf.
static inline void biquad_lowshelf(Biquad *bq, float fs, float f0, float q, float gain_db) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * TT_BIQUAD_PI * f0 / fs;
    float cw    = cosf(w0);
    float sw    = sinf(w0);
    float alpha = sw / (2.0f * q);
    float ta    = 2.0f * sqrtf(A) * alpha;
    float a0    =        (A + 1.0f) + (A - 1.0f) * cw + ta;
    bq->b0 =    A * ( (A + 1.0f) - (A - 1.0f) * cw + ta ) / a0;
    bq->b1 = 2.0f * A * ( (A - 1.0f) - (A + 1.0f) * cw ) / a0;
    bq->b2 =    A * ( (A + 1.0f) - (A - 1.0f) * cw - ta ) / a0;
    bq->a1 = -2.0f * ( (A - 1.0f) + (A + 1.0f) * cw )    / a0;
    bq->a2 =        ( (A + 1.0f) + (A - 1.0f) * cw - ta ) / a0;
    biquad_reset(bq);
}

// RBJ high shelf — boost/cut everything above ~f0.
static inline void biquad_highshelf(Biquad *bq, float fs, float f0, float q, float gain_db) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * TT_BIQUAD_PI * f0 / fs;
    float cw    = cosf(w0);
    float sw    = sinf(w0);
    float alpha = sw / (2.0f * q);
    float ta    = 2.0f * sqrtf(A) * alpha;
    float a0    =        (A + 1.0f) - (A - 1.0f) * cw + ta;
    bq->b0 =    A * ( (A + 1.0f) + (A - 1.0f) * cw + ta ) / a0;
    bq->b1 = -2.0f * A * ( (A - 1.0f) + (A + 1.0f) * cw ) / a0;
    bq->b2 =    A * ( (A + 1.0f) + (A - 1.0f) * cw - ta ) / a0;
    bq->a1 = 2.0f * ( (A - 1.0f) - (A + 1.0f) * cw )     / a0;
    bq->a2 =        ( (A + 1.0f) - (A - 1.0f) * cw - ta ) / a0;
    biquad_reset(bq);
}

#endif // TT_BIQUAD_H