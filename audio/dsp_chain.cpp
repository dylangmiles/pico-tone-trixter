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
    { "level", 0.30f, 0.0f, 2.0f, 0.01f, "x" },   // dialed for K&K slide: comp-in peaks ~-3 dBFS
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
// Stage 0: IR — a flag-only stage. The actual 2048-tap convolution runs cross-core
// in es8388_test.cpp BEFORE dsp_chain_process(), so process()/recompute() are no-ops
// here; this entry exists so the IR shows in `dump`, toggles via `ir on|off`, and is
// covered by global bypass uniformly with the rest. es8388_test reads its effective
// state via dsp_chain_ir_enabled().
static void ir_noop_recompute(Stage *s, float fs) { (void)s; (void)fs; }
static void ir_noop_process(Stage *s, float *buf, int n) { (void)s; (void)buf; (void)n; }

static Stage s_ir   = { "ir",   true,  NULL,         0,     false, ir_noop_recompute, ir_noop_process, NULL };
static Stage s_in   = { "in",   true,  s_in_params,  IN_N,  true,  in_recompute,   in_process,   &s_in_state  };
static Stage s_eq   = { "eq",   true,  s_eq_params,  EQ_N,  true,  eq_recompute,   eq_process,   &s_eq_state  };
static Stage s_comp = { "comp", true,  s_cp_params,  CP_N,  true,  comp_recompute, comp_process, &s_cp_state  };
static Stage s_out  = { "out",  true,  s_out_params, OUT_N, true,  out_recompute,  out_process,  &s_out_state };

static Stage *s_chain[] = { &s_ir, &s_in, &s_eq, &s_comp, &s_out };
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
    // name              ir             in    eq     cmp   out   in    lo_f lo_g mid_f  mid_g  mid_q hi_f   hi_g  thr  ratio att rel  mkup out
    // default: dry baseline — no IR (ir NULL ⇒ "none" ⇒ convolution off). The inheritance seed.
    { "default",          NULL,         true, false, true, true, 0.30f, 120,  0,  700,  0.0f,  1.0f, 3500,  0.0f, -20, 2.0f, 20, 200,  3,  0.80f },
    { "tanglewood-slide", "tanglewood", true, true,  true, true, 0.30f, 150,  3,  800, -3.0f,  1.0f, 1500, -2.0f, -16, 3.5f, 22, 300,  6,  0.70f },
    // garrison (active pre-amp): EQ on by default — gentle warmth + de-honk scoop + top roll-off
    { "garrison",         "garrison",   true, true,  true, true, 0.30f, 120,  2, 1000, -2.5f,  1.0f, 3500, -1.5f, -18, 3.0f, 20, 250,  5,  0.70f },
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
           "  in.level <x>            pre-comp trim — tame the hot IR (meter to ~-3 dBFS)\n"
           "  bypass on|off           kill IR+EQ+Dynamics (output level only)\n"
           "  ir on|off               IR convolution on/off (independent of bypass)\n"
           "  tuner on|off            guitar tuner (UART needle; dry monitor)\n"
           "  meter on|off            live compressor gain-reduction readout (~1/s)\n"
           "  dump                    list all stages + params\n"
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