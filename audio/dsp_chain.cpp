// audio/dsp_chain.cpp — see dsp_chain.h for the design rationale.
//
// Chain (post-IR):  EQ (3-band) -> Dynamics (comp/limiter/sustain) -> Output level
//
// EQ + Dynamics boot DISABLED and Output level boots at the legacy IR_OUTPUT_SCALE,
// so a fresh boot sounds bit-identical to the pre-chain firmware until you enable a
// stage over UART. Coefficients recompute only when a param changes (dirty flag),
// and biquad/compressor state is preserved across recompute so you can sweep a knob
// while playing without clicks.

#include "audio/dsp_chain.h"
#include "audio/biquad.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

volatile bool g_dsp_bypass = false;

static float g_fs = 48000.0f;

// ---------------------------------------------------------------------------
// Stage 1: 3-band EQ — low shelf, mid peak (sweepable freq/Q), high shelf
// ---------------------------------------------------------------------------
enum { EQ_LO_F, EQ_LO_G, EQ_MID_F, EQ_MID_G, EQ_MID_Q, EQ_HI_F, EQ_HI_G, EQ_N };

static Param s_eq_params[EQ_N] = {   // voiced for warm open-G acoustic slide (K&K), dialed 2026-06-08
    { "lo_freq",   150.0f,   40.0f,   500.0f,   5.0f, "Hz" },   // +3 dB shelf: body/warmth
    { "lo_gain",     3.0f,  -15.0f,    15.0f,   0.5f, "dB" },
    { "mid_freq",  800.0f,  200.0f,  4000.0f,  10.0f, "Hz" },   // -3 dB scoop: de-honk, vocal mids
    { "mid_gain",   -3.0f,  -15.0f,    15.0f,   0.5f, "dB" },
    { "mid_q",       1.0f,    0.3f,     4.0f,   0.1f, "Q"  },
    { "hi_freq",  1500.0f, 1500.0f, 12000.0f,  50.0f, "Hz" },   // -2 dB shelf: roll off slide scrape
    { "hi_gain",    -2.0f,  -15.0f,    15.0f,   0.5f, "dB" },
};

typedef struct { Biquad lo, mid, hi; } EqState;
static EqState s_eq_state;

static void eq_recompute(Stage *s, float fs) {
    EqState *st = (EqState *)s->state;
    const Param *p = s->params;
    // Preserve filter memory across retune so live knob sweeps don't click.
    float lz1 = st->lo.z1,  lz2 = st->lo.z2;
    float mz1 = st->mid.z1, mz2 = st->mid.z2;
    float hz1 = st->hi.z1,  hz2 = st->hi.z2;
    biquad_lowshelf (&st->lo,  fs, p[EQ_LO_F].value,  0.707f,           p[EQ_LO_G].value);
    biquad_peaking  (&st->mid, fs, p[EQ_MID_F].value, p[EQ_MID_Q].value, p[EQ_MID_G].value);
    biquad_highshelf(&st->hi,  fs, p[EQ_HI_F].value,  0.707f,           p[EQ_HI_G].value);
    st->lo.z1 = lz1;  st->lo.z2 = lz2;
    st->mid.z1 = mz1; st->mid.z2 = mz2;
    st->hi.z1 = hz1;  st->hi.z2 = hz2;
}

static void eq_process(Stage *s, float *buf, int n) {
    EqState *st = (EqState *)s->state;
    for (int i = 0; i < n; i++) {
        float x = buf[i];
        x = biquad_process(&st->lo,  x);
        x = biquad_process(&st->mid, x);
        x = biquad_process(&st->hi,  x);
        buf[i] = x;
    }
}

// ---------------------------------------------------------------------------
// Stage 2: Dynamics — feed-forward peak compressor (comp / limiter / sustain)
//   sustain  = high ratio + long release
//   limiter  = very high ratio + fast attack + high threshold
// ---------------------------------------------------------------------------
enum { CP_THRESH, CP_RATIO, CP_ATT, CP_REL, CP_MAKEUP, CP_N };

static Param s_cp_params[CP_N] = {   // defaults tuned for acoustic bottleneck slide (K&K)
    { "thresh",  -16.0f, -60.0f,    0.0f,  1.0f, "dB" },
    { "ratio",     3.5f,   1.0f,   20.0f,  0.5f, ":1" },
    { "attack",   22.0f,   1.0f,  100.0f,  1.0f, "ms" },
    { "release", 300.0f,  10.0f, 1000.0f, 10.0f, "ms" },
    { "makeup",    6.0f, -12.0f,   24.0f,  0.5f, "dB" },
};

typedef struct {
    float env;          // detector envelope (preserved across recompute)
    float gain;         // current gain factor (held between control-rate updates)
    float att, rel;     // one-pole coeffs
    float thr_lin;      // threshold, linear
    float slope;        // 1/ratio - 1  (<= 0)
    float makeup_lin;
} CompState;
static CompState s_cp_state;
static float     s_comp_gr_peak = 1.0f;   // min comp gain since last meter read (1.0 = no reduction)
static float     s_comp_in_peak = 0.0f;   // peak |input| to the comp since last meter read

// Recompute the gain only every CP_CTRL samples. The envelope moves on ms time
// constants, so a ~6 kHz gain update (48k/8) is inaudible but avoids a per-sample
// powf — the per-sample transcendental was blowing the block budget (stutter).
#define CP_CTRL 8

static void comp_recompute(Stage *s, float fs) {
    CompState *st = (CompState *)s->state;
    const Param *p = s->params;
    st->att        = expf(-1.0f / (fs * p[CP_ATT].value * 0.001f));
    st->rel        = expf(-1.0f / (fs * p[CP_REL].value * 0.001f));
    st->thr_lin    = powf(10.0f, p[CP_THRESH].value / 20.0f);
    st->slope      = (1.0f / p[CP_RATIO].value) - 1.0f;
    st->makeup_lin = powf(10.0f, p[CP_MAKEUP].value / 20.0f);
}

static void comp_process(Stage *s, float *buf, int n) {
    CompState *st = (CompState *)s->state;
    float env  = st->env;
    float gain = st->gain;
    const float thr    = st->thr_lin;
    const float slope  = st->slope;
    const float makeup = st->makeup_lin;
    for (int i = 0; i < n; i++) {
        float x = buf[i];
        float a = fabsf(x);
        if (a > s_comp_in_peak) s_comp_in_peak = a;   // meter: peak input level
        // Peak detector: fast attack toward rising peaks, slow release. Per-sample.
        if (a > env) env = st->att * env + (1.0f - st->att) * a;
        else         env = st->rel * env + (1.0f - st->rel) * a;
        // Control-rate gain: (env/thr)^(1/ratio - 1) — one powf instead of the old
        // log10f + powf, and only every CP_CTRL samples.
        if ((i & (CP_CTRL - 1)) == 0) {
            gain = (env > thr && env > 1e-9f) ? powf(env / thr, slope) : 1.0f;
            if (gain < s_comp_gr_peak) s_comp_gr_peak = gain;   // meter: track peak reduction
        }
        buf[i] = x * gain * makeup;
    }
    st->env  = env;
    st->gain = gain;
}

// ---------------------------------------------------------------------------
// Stage 3: Output level (replaces the fixed IR_OUTPUT_SCALE)
// ---------------------------------------------------------------------------
enum { OUT_LEVEL, OUT_N };

static Param s_out_params[OUT_N] = {
    { "level", 0.7f, 0.0f, 2.0f, 0.05f, "x" },   // 0.7 ≈ legacy IR_OUTPUT_SCALE; up to +6 dB
};

typedef struct { float lin; } OutState;
static OutState s_out_state;

static void out_recompute(Stage *s, float fs) {
    (void)fs;
    ((OutState *)s->state)->lin = s->params[OUT_LEVEL].value;
}

static void out_process(Stage *s, float *buf, int n) {
    float lin = ((OutState *)s->state)->lin;
    for (int i = 0; i < n; i++) buf[i] *= lin;
}

// ---------------------------------------------------------------------------
// Input trim — scales the (hot) IR output DOWN before EQ/comp, so the comp sees a
// sane level and nothing clips even comp-off. The IR adds ~25 dB on transients
// (peaks hit +12 dBFS); dial in.level on the meter so `comp in` peaks ~-3 dBFS.
// ---------------------------------------------------------------------------
enum { IN_LEVEL, IN_N };
static Param s_in_params[IN_N] = {
    // Max raised 2.0 -> 8.0 (+18 dB) on 2026-07-24. Was an attenuator-only range, sized when the
    // job was purely "tame the hot IR" with the analog chain making up level ahead of the ADC.
    // Two measurements inverted that: the ADC clips at 1.35 V pk (Test C) so `pga` must run at 0,
    // and gain before the ADC buys no SNR (SNR session Block 3: +18 dB pga -> +17.4 dB noise), so
    // the makeup belongs AFTER the converter where it's free. Restoring op-amp-era staging on the
    // JFET daughter needs x2.5 (garrison) to x5.0 (tanglewood-slide) -- both past the old cap.
    // Step 0.01 -> 0.05: at the new 3.0-4.5 working values that's ~0.1 dB/detent (it was 800
    // detents end-to-end at 0.01). Nothing downstream assumes <=1.0 -- comp + out.level and the
    // int32 clip in main.cpp bound the DAC -- but this stage can now push the comp hard, so dial
    // it on `meter` (comp-in peaks ~-3 dBFS), not by ear alone.
    { "level", 0.30f, 0.0f, 8.0f, 0.05f, "x" },   // dialed for K&K slide: comp-in peaks ~-3 dBFS
};
typedef struct { float lin; } InState;
static InState s_in_state;
static void in_recompute(Stage *s, float fs) {
    (void)fs;
    ((InState *)s->state)->lin = s->params[IN_LEVEL].value;
}
static void in_process(Stage *s, float *buf, int n) {
    float lin = ((InState *)s->state)->lin;
    for (int i = 0; i < n; i++) buf[i] *= lin;
}

// ---------------------------------------------------------------------------
// Chain wiring
// ---------------------------------------------------------------------------
// IR enable — a flag only, NOT a chain stage. The 2048-tap convolution runs cross-core
// in main.cpp BEFORE dsp_chain_process(), gated by dsp_chain_ir_enabled(). It's driven
// entirely by the host's IR SELECTION ("none" vs a real IR — the `ir <name|none>` command,
// preset, or encoder IR picker, via dsp_chain_set_ir_enabled), so it is deliberately kept
// OUT of s_chain: there's no "ir on/off" toggle in the stage menu / `dump` (that would be a
// redundant second control). Global bypass still disables it (see dsp_chain_ir_enabled).
static Stage s_ir   = { "ir",   true,  NULL,         0,     false, NULL, NULL, NULL };
static Stage s_in   = { "in",   true,  s_in_params,  IN_N,  true,  in_recompute,   in_process,   &s_in_state  };
static Stage s_eq   = { "eq",   true,  s_eq_params,  EQ_N,  true,  eq_recompute,   eq_process,   &s_eq_state  };
static Stage s_comp = { "comp", true,  s_cp_params,  CP_N,  true,  comp_recompute, comp_process, &s_cp_state  };
static Stage s_out  = { "out",  true,  s_out_params, OUT_N, true,  out_recompute,  out_process,  &s_out_state };

// Audio stage chain (post-IR): IN trim -> EQ -> Dynamics -> Output. s_ir is intentionally
// excluded (it's a selection, not a toggleable stage) — see above.
static Stage *s_chain[] = { &s_in, &s_eq, &s_comp, &s_out };
#define N_STAGES ((int)(sizeof(s_chain) / sizeof(s_chain[0])))

int    dsp_chain_stage_count(void) { return N_STAGES; }
Stage *dsp_chain_stage(int i)      { return (i >= 0 && i < N_STAGES) ? s_chain[i] : NULL; }

// Effective IR state for es8388_test's foreground loop: on only if the IR stage is
// enabled AND global bypass is off.
bool dsp_chain_ir_enabled(void) { return s_ir.enabled && !g_dsp_bypass; }

// Host-driven IR stage enable (main.cpp sets this from the current IR selection:
// off when "none" is selected, on when a real IR is loaded).
void dsp_chain_set_ir_enabled(bool on) { s_ir.enabled = on; }

// Peak compressor gain reduction (dB, <= 0) since the last call; resets each call so
// a periodic reader gets "peak GR over the interval". 0 dB = not compressing.
float dsp_chain_comp_gr_db(void) {
    float g = s_comp_gr_peak;
    s_comp_gr_peak = 1.0f;
    if (g >= 1.0f) return 0.0f;
    if (g < 1e-6f) g = 1e-6f;
    return 20.0f * log10f(g);
}

// Peak input level to the compressor (dBFS) since the last call; resets each call.
// Lets you set comp.thresh relative to the actual signal hitting the stage.
float dsp_chain_comp_in_db(void) {
    float p = s_comp_in_peak;
    s_comp_in_peak = 0.0f;
    if (p < 1e-6f) return -120.0f;
    return 20.0f * log10f(p);
}

// ---------------------------------------------------------------------------
// Presets — IR ref + every stage's params/enables. `ir` is resolved by the host
// (main.cpp) to a convolver IR: a built-in name or an SD WAV filename.
// The active list defaults to s_builtin, but dsp_chain_install_presets() can point
// it at an SD-parsed table (see audio/tt_store).
// ---------------------------------------------------------------------------
static const Preset s_builtin[] = {
    // ===== 2026-07-24 RESTAGE FOR THE JFET DAUGHTER AT `pga 0` — FIRST PASS, NOT YET TUNED BY EAR =====
    // Every preset below was dialed on the OPA1642 op-amp daughter (x2.11). Two measurements forced a
    // restage: (1) Test C — the ADC clips at 1.35 V pk, so `pga` must sit at 0; (2) the SNR session —
    // gain ahead of the ADC buys no SNR (+18 dB pga -> +17.4 dB noise), so makeup belongs after it.
    //
    // Two independent level changes, applied to every preset:
    //   a) daughter swap  x2.11 -> x0.84  = -8.0 dB  (hits all three presets equally)
    //   b) pga -> 0       = -6 dB (tanglewood) / -12 dB (default) / 0 dB (garrison, already floored)
    //
    // in.level is NOT set to restore the old RMS — that would clip. It is set to hold the old
    // comp-input PEAK, because the peaks changed differently from the sustain: at the old staging the
    // ADC was hard-clipping every transient (a hard strum ran +11.7 dB over at pga 6, +17.8 at pga 12),
    // so the converter was silently acting as the peak limiter. At `pga 0` through the JFET that same
    // strum lands at -2.25 dBFS, intact. So the peak arriving at the comp is ~2.2 dB LOWER than before
    // (it was pinned to 0 dBFS by clipping), while the SUSTAIN dropped by the full a+b. in.level takes
    // the small peak correction; comp.thresh/makeup take the large sustain correction.
    //
    //   preset             in.level      comp.thr      makeup    reason
    //   default            0.30 -> 0.40  -20 -> -37    3  -> 20  sustain -17.5 dB (pga -12, daughter -8)
    //   tanglewood-slide   0.90 -> 1.15  -22 -> -34    6  -> 18  sustain -11.8 dB (pga -6,  daughter -8)
    //   garrison           1.20 -> 3.00  -22 (same)    5 (same)  pga unchanged => pure x2.512 bookkeeping
    //
    // The K&K presets are derived from measured numbers and should land close. GARRISON'S in.level 3.00
    // is exact gain bookkeeping (same total chain gain as before) but assumes the active preamp was
    // previously at/near ADC clip — its actual output has never been measured. Check `meter` first.
    // Attack times cut ~30% and out.level trimmed on all three: the ADC no longer limits transients for
    // us, so the comp has to catch what the converter used to flatten. Dial on `meter` (comp-in peaks
    // ~-3 dBFS, 0 DAC clips), not by ear alone.
    //
    // name              ir             in    eq     cmp   out   in    lo_f lo_g mid_f  mid_g  mid_q hi_f   hi_g  thr  ratio att rel  mkup out    pga
    // default: dry baseline — no IR (ir NULL ⇒ "none" ⇒ convolution off). Three jobs, all of which
    // want NEUTRAL AND SAFE rather than tuned: (1) the inheritance seed — a partial SD preset silently
    // inherits every key it omits from here; (2) the dry A/B reference you switch to in order to hear
    // what the IR/EQ/comp are actually doing; (3) the power-on preset.
    //
    // Revised 2026-07-24 after the Garrison bench session. The previous values (in.level 0.40,
    // thr -37, makeup 20) came from the same first-pass arithmetic that the Garrison proved ~12 dB
    // wrong — it restored the old RMS through a stage that had been CLIPPING, which is not a valid
    // gain calculation. They also made the "dry baseline" the most heavily compressed preset in the
    // file (20 dB of makeup squashing 30 dB of range), which is the opposite of a baseline.
    //
    // - in.level 1.00: unity. An honest seed, and it means comp-in == ADC level on the dry path.
    // - thr -18 / ratio 3 / makeup 6: light SAFETY limiting, not sustain. Manufacturing loudness is
    //   the tuned presets' job; this one should sound like the guitar.
    // - attack 1 ms: THE generalizable lesson. The ADC used to flatten every transient for free at
    //   the old pga values; at pga 0 nothing does, and the Garrison showed even 3 ms let the leading
    //   edge through to the DAC. Anything inheriting from here must inherit a fast attack.
    // Predicted dry, K&K or Garrison at their reference level (ADC peak ~-2.5 dBFS): comp-in -2.5,
    // GR ~10 dB, out ~-10 dBFS, ~10 dB of DAC margin. To be confirmed on both guitars.
    // Confirmed on the Garrison 2026-07-24 (preamp 3/4, dry): comp-in peak -1.1 dBFS, GR -5..-7,
    // out peak -4.0, clip[ADC 0 DAC 0]. out.level 0.70 -> 0.60 because that take was still ~5 dB
    // under the hardest recorded strum (+4.2 comp-in), which extrapolates to out ~-2 dBFS. 1.3 dB is
    // cheap insurance on the preset every partial SD preset inherits from.
    //
    // Also confirmed on the K&K same day, and this preset is now the standard ADC-headroom probe:
    // dry + in.level unity + eq off means **comp in IS the ADC level**, no unknown IR gain in the
    // way. Use `preset default` whenever you need to know where the converter actually sits.
    // K&K hard body tap + two very hard bridge taps: comp in -1.1 / 0.0 / 0.0 dBFS, clip[ADC 0].
    // DAC clipped 1/7/12 samples (0.02-0.25 ms) and the builder could not hear it, so 0.60 stands.
    // That clipping is pure overshoot and is NOT tunable: 0.0 (comp in) + 6.0 (makeup) - 4.4
    // (out.level 0.60) = +1.6 dBFS whenever the comp contributes nothing, and a 1 ms attack CANNOT
    // catch a knuckle tap -- the leading edge arrives before any GR exists. Only static headroom
    // fixes it (out.level 0.35 would), which is not worth the level on a preset this quiet.
    { "default",          NULL,         true, false, true, true, 1.00f, 120,  0,  700,  0.0f,  1.0f, 3500,  0.0f, -18, 3.0f,  1, 250,  6,  0.60f,  0 },
    // tanglewood-slide (passive K&K, JFET daughter): pga 0 — the JFET's x0.84 puts the ADC ceiling at
    // 1.60 V at TSin, which clears a hard strum (1.24 V) with 2.2 dB spare; only body taps clip now.
    //
    // DIALED ON THE K&K 2026-07-24, and it is the only preset today whose first pass landed --
    // because its starting values were the Garrison's MEASURED numbers rather than fresh arithmetic
    // (shared front end: same JFET daughter, same pga 0, similar ADC peak). Only out.level moved,
    // 0.50 -> 0.65, because the builder wanted "a touch louder". Everything else is untouched.
    // The values this replaced (in.level 1.15, thr -34, att 12, makeup 18) were first-pass arithmetic
    // of exactly the kind the Garrison session disproved -- derived by restoring the old RMS through
    // the ADC, which had been ~11.7 dB into clipping at the old pga 6. They predicted comp-in ~+8 dBFS
    // and would have clipped the DAC on the first strum.
    //
    // Measured over a full slide passage: GR -6..-14 dB and NEVER 0 (the comp rides the level all the
    // way through rather than only catching attacks -- what a slide preset is for), comp in swinging
    // -5..-17 dBFS comes out -7..-13, peak comp in -1.8 -> out -6.3 dBFS, clip[ADC 0 DAC 0] over the
    // whole take. At out.level 0.65 that peak sits ~-4.0 dBFS, matching the Garrison's margin.
    //
    // Two K&K-specific facts, both the OPPOSITE of what was expected going in:
    //  - **No pad needed.** Its worst case (very hard bridge tap) measures 0.0 dBFS at the ADC with
    //    ZERO clips -- 4.9..7.0 dB better than the headroom session predicted. Probably the JFET's
    //    asymmetric window soft-limiting the positive half at +1.2 V before the converter sees it.
    //    Body taps still clip the DAC by a handful of samples here (inaudible, accepted).
    //  - **The IR, not the front end, sets the hiss.** Output noise floor -42.5 dBFS vs the
    //    Garrison's -49.4 at identical in.level/makeup/out.level, even though the ADC noise floor
    //    measures the same for both guitars (~-53..-55 dBFS on `default`). Back-derived: the
    //    tanglewood IR is +3 dB on transients and +3.5 dB on broadband noise (flat, passes hiss); the
    //    garrison IR is +9 dB on transients and -2 dB on noise (peaky, suppresses it). No digital
    //    gain can fix this -- in.level/makeup move signal and noise together. Levers are gate-node
    //    shielding or acceptance. Builder finds it noticeable but acceptable.
    { "tanglewood-slide", "tanglewood", true, true,  true, true, 0.75f, 150,  3,  800, -3.0f,  1.0f, 1500, -2.0f, -28, 4.0f,  1, 300, 16,  0.65f,  0 },
    // garrison (active pre-amp) — DIALED AT THE BENCH 2026-07-24 on the JFET daughter at pga 0.
    //
    // *** REFERENCE CONDITION: guitar preamp at 3/4. *** Record it with any Garrison measurement or
    // the numbers are not reproducible -- the guitar's own volume sits AHEAD of the whole pedal, so a
    // preset is only valid for one knob position.
    //
    // 3/4 is not arbitrary, it is the measured optimum, and how it was found is the one genuinely
    // counter-intuitive result of the session. The guitar's preamp is the ONLY gain control in the
    // chain sitting ahead of the pedal's dominant noise sources (JFET gate, ADC), so it is the only
    // one that improves SNR. Measured at comp in (pre-comp, so comp settings cannot confound it):
    //
    //   preamp     comp-in peak   noise floor   SNR       ADC
    //   half         -6.2 dBFS     -61.8        55.6 dB   clean
    //   3/4          +4.2          -59.7        63.9 dB   clean      <- here
    //   near max     +7.8          -58.4        66.2 dB   7 samples clipped
    //
    // half -> 3/4 buys +8.4 dB of real SNR (signal +10.4, noise only +2.1); 3/4 -> near max buys just
    // +2.3 dB more and starts clipping the converter. Diminishing returns land almost exactly on 3/4.
    // Gain anywhere AFTER the front end -- pga, in.level, comp makeup -- lifts signal and noise
    // together and can never do this (SNR session Block 3: +18 dB pga -> +17.4 dB noise). So run the
    // guitar hot, back off only far enough that hard playing stays clean, and make up nothing analog.
    // At FULL a hard strum reaches 1.27-1.60 V at TSin and clips the ADC properly (51 samples).
    //
    // Measured at these values, preamp 3/4: comp in peak +4.2 dBFS (ADC peak -2.5, 2.5 dB of
    // converter margin), GR -22 on the attack tapering -13 -> -4 -> 0 across the decay, out peak
    // -3.4 dBFS, clip[ADC 0 DAC 0]. Noise floor -59.7 at comp in, -49.4 at out.
    // Limiting behaviour verified at near-max preamp: a body tap at comp in +0.1 and a very hard
    // strum at +7.8 both produced out ~-4.2 dBFS -- +7.7 dB of input, +0.2 dB of output. The comp is
    // limiting, not just levelling, which is why out.level 0.50 has never produced a DAC clip in any
    // take (body tap, hard strum, all three preamp positions).
    // NOTE the Garrison's worst case is the STRUM, not the body tap (the tap came in 7.7 dB lower) --
    // the opposite of the K&K. Active under-saddle vs soundboard transducer; don't carry the K&K's
    // "worst case is a body tap" rule across ([[feedback_transient_peak_capture_method]] is about
    // soundboard piezos specifically).
    //
    // How the values got here, since the first pass was wrong in an instructive way:
    // - in.level 3.00 -> 0.75. The x2.512 "restore the old chain gain" bookkeeping ran THROUGH A
    //   CLIPPING STAGE: on the op-amp daughter this guitar was ~8 dB into ADC clipping on every hard
    //   strum, so its peak was pinned at 0 dBFS and did not scale with the daughter swap the way the
    //   arithmetic assumed. Gain bookkeeping is only valid through linear stages.
    // - attack 14 -> 1 ms. The ADC used to flatten these transients for free; at 14 ms (and still at
    //   3) the leading edge escaped the detector and hit the DAC. 1 ms was worth ~3 dB of GR.
    // - thr -20 -> -28, ratio 6 -> 4, makeup 6 -> 16. At -20 the comp only caught the initial
    //   transient (GR 0.0 for the whole decay). At -28 GR holds ~-22 through the first second and
    //   tapers -10 -> -5 -> 0: input range 43 dB becomes output range 14 dB. That is the sustainer.
    // - out.level 0.85 -> 0.50. At 0.85 the output peaked -0.1 dBFS -- zero DAC clips by luck, no
    //   margin. 0.50 puts peaks at -4.2 with the sustain still well above the earlier inaudible take.
    // Builder accepted the higher hiss (compression lifts the noise floor with the quiet parts) as a
    // fair trade for the sustain; the JFET's ~5 dB mains excess is a shielding fix, not device noise.
    { "garrison",         "garrison",   true, true,  true, true, 0.75f, 120,  2, 1000, -2.5f,  1.0f, 3500, -1.5f, -28, 4.0f,  1, 250, 16,  0.50f,  0 },
};
#define N_BUILTIN ((int)(sizeof(s_builtin) / sizeof(s_builtin[0])))

static const Preset *s_presets = s_builtin;   // active list (built-in or SD-installed)
static int           s_n_presets = N_BUILTIN;

void dsp_chain_install_presets(const Preset *arr, int n) {
    if (arr && n > 0) { s_presets = arr;      s_n_presets = n; }
    else              { s_presets = s_builtin; s_n_presets = N_BUILTIN; }
}

const Preset *dsp_chain_default_preset(void) {
    for (int i = 0; i < N_BUILTIN; i++)
        if (strcmp(s_builtin[i].name, "default") == 0) return &s_builtin[i];
    return &s_builtin[0];
}

int         dsp_chain_preset_count(void)     { return s_n_presets; }
const char *dsp_chain_preset_name(int idx)   { return (idx >= 0 && idx < s_n_presets) ? s_presets[idx].name : ""; }
const char *dsp_chain_preset_ir(int idx) {
    const char *ir = (idx >= 0 && idx < s_n_presets) ? s_presets[idx].ir : NULL;
    return ir ? ir : "";
}
int dsp_chain_preset_pga(int idx) {
    return (idx >= 0 && idx < s_n_presets) ? s_presets[idx].pga : -1;   // <0 = leave PGA unchanged
}

int dsp_chain_find_preset(const char *name) {
    for (int i = 0; i < s_n_presets; i++)
        if (strcmp(s_presets[i].name, name) == 0) return i;
    return -1;
}

int dsp_chain_load_preset(int idx) {
    if (idx < 0 || idx >= s_n_presets) return -1;
    const Preset *p = &s_presets[idx];
    // Note: the IR stage enable is driven by the host's IR selection (none vs a real IR),
    // not set here — see main.cpp app_preset_load / the IR switch.
    s_in.enabled = p->in_on;  s_eq.enabled = p->eq_on;
    s_comp.enabled = p->comp_on;  s_out.enabled = p->out_on;
    s_in_params[IN_LEVEL].value  = p->in_level;
    s_eq_params[EQ_LO_F].value   = p->lo_f;  s_eq_params[EQ_LO_G].value  = p->lo_g;
    s_eq_params[EQ_MID_F].value  = p->mid_f; s_eq_params[EQ_MID_G].value = p->mid_g;
    s_eq_params[EQ_MID_Q].value  = p->mid_q;
    s_eq_params[EQ_HI_F].value   = p->hi_f;  s_eq_params[EQ_HI_G].value  = p->hi_g;
    s_cp_params[CP_THRESH].value = p->thr;   s_cp_params[CP_RATIO].value = p->ratio;
    s_cp_params[CP_ATT].value    = p->att;   s_cp_params[CP_REL].value   = p->rel;
    s_cp_params[CP_MAKEUP].value = p->mkup;
    s_out_params[OUT_LEVEL].value = p->out_level;
    s_in.dirty = s_eq.dirty = s_comp.dirty = s_out.dirty = true;
    return 0;
}

void dsp_chain_init(float fs, float out_level_lin) {
    g_fs = fs;
    s_out_params[OUT_LEVEL].value = out_level_lin;
    s_cp_state.env  = 0.0f;
    s_cp_state.gain = 1.0f;
    for (int i = 0; i < N_STAGES; i++) {
        s_chain[i]->recompute(s_chain[i], fs);
        s_chain[i]->dirty = false;
    }
}

void dsp_chain_process(float *buf, int n) {
    for (int i = 0; i < N_STAGES; i++) {
        Stage *s = s_chain[i];
        bool on = s->enabled;
        if (g_dsp_bypass && s != &s_out) on = false;   // bypass = EQ+Dyn off, level stays
        if (!on) continue;
        if (s->dirty) { s->recompute(s, g_fs); s->dirty = false; }
        s->process(s, buf, n);
    }
}

// ---------------------------------------------------------------------------
// UART command interface (Core 0 only). Same model the encoder/menu will drive.
// ---------------------------------------------------------------------------
static Stage *find_stage(const char *name) {
    for (int i = 0; i < N_STAGES; i++)
        if (strcmp(s_chain[i]->name, name) == 0) return s_chain[i];
    return NULL;
}

static Param *find_param(Stage *s, const char *name) {
    for (int i = 0; i < s->n_params; i++)
        if (strcmp(s->params[i].name, name) == 0) return &s->params[i];
    return NULL;
}

static void chain_dump(void) {
    printf("--- DSP chain (bypass=%s) ---\n", g_dsp_bypass ? "on" : "off");
    for (int i = 0; i < N_STAGES; i++) {
        Stage *s = s_chain[i];
        printf("[%s] %s\n", s->name, s->enabled ? "on" : "off");
        for (int j = 0; j < s->n_params; j++) {
            Param *p = &s->params[j];
            printf("    %s.%-8s = %8.3g %-3s  [%g..%g]\n",
                   s->name, p->name, (double)p->value, p->unit,
                   (double)p->vmin, (double)p->vmax);
        }
    }
}

static void chain_help(void) {
    printf("DSP commands:\n"
           "  <stage>.<param> <val>   set param   (e.g. eq.mid_gain 3.5)\n"
           "  <stage>.<param>         show param\n"
           "  <stage> on|off          enable/bypass stage (in, eq, comp, out)\n"
           "  preset [name]           list presets, or load one (switches IR too)\n"
           "  in.level <x>            pre-comp trim + post-ADC makeup, 0..8 (meter to ~-3 dBFS)\n"
           "  bypass on|off           raw ADC->DAC: only out.level applies (IR/EQ/comp/in.level all off)\n"
           "  ir [name|none]          select IR: a name (tanglewood/garrison/an SD .wav), none=off,\n"
           "                          on=re-engage; no arg shows current + lists options\n"
           "  tuner on|off            guitar tuner (UART needle; dry monitor)\n"
           "  meter on|off            live level meter: comp in / GR / output dBFS / ADC+DAC clip (~1/s)\n"
           "  dump                    app/codec settings (pga, ir, preset...) + all stages + params\n"
           "  stats                   block / timing counters\n"
           "  help                    this\n");
}

bool dsp_chain_command(const char *line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // Trim trailing CR/LF/space and leading space.
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' '))
        buf[--len] = '\0';
    char *p = buf;
    while (*p == ' ') p++;
    if (*p == '\0') return false;

    // Split off the argument at the first space.
    char *arg = strchr(p, ' ');
    if (arg) { *arg = '\0'; arg++; while (*arg == ' ') arg++; }

    if (strcmp(p, "help") == 0) { chain_help(); return true; }
    if (strcmp(p, "dump") == 0) { chain_dump(); return true; }
    if (strcmp(p, "bypass") == 0) {
        if (arg) { g_dsp_bypass = (strcmp(arg, "on") == 0); }
        printf("bypass=%s\n", g_dsp_bypass ? "on" : "off");
        return true;
    }

    char *dot = strchr(p, '.');
    if (dot) {
        *dot = '\0';
        const char *sname = p, *pname = dot + 1;
        Stage *s = find_stage(sname);
        if (!s) { printf("? no stage '%s'\n", sname); return true; }
        Param *pr = find_param(s, pname);
        if (!pr) { printf("? no param '%s.%s'\n", sname, pname); return true; }
        if (arg) {
            float v = strtof(arg, NULL);
            if (v < pr->vmin) v = pr->vmin;
            if (v > pr->vmax) v = pr->vmax;
            pr->value = v;
            s->dirty = true;
        }
        printf("%s.%s=%.3g %s\n", sname, pname, (double)pr->value, pr->unit);
        return true;
    }

    // Bare stage token -> enable/bypass. Accept ONLY on/off so a malformed command
    // (e.g. "out level 0.2" with a space instead of a dot, or a stray serial byte)
    // can't silently disable a stage.
    Stage *s = find_stage(p);
    if (!s) return false;
    if (arg) {
        if (strcmp(arg, "on") == 0)       s->enabled = true;
        else if (strcmp(arg, "off") == 0) s->enabled = false;
        else { printf("? '%s' takes on|off (got '%s') — to set a param use '%s.<param> <val>'\n",
                      p, arg, p); return true; }
    }
    printf("%s=%s\n", p, s->enabled ? "on" : "off");
    return true;
}