// audio/dsp_chain.h — configurable, menu-ready DSP stage chain for Tone Trixter.
//
// Design goals (set 2026-06-07):
//   - Every effect is a Stage; every tunable is a Param (name/value/min/max/step).
//   - Stages run in series on the mono block, each independently enable/bypassable.
//   - A future encoder + OLED menu walks the SAME Param/Stage model that the UART
//     command interface drives today — so the control surface is swappable, not
//     rewritten.
//   - Param changes are real-time safe: a change marks the stage dirty and it
//     recomputes coefficients once at the next block boundary (never per-sample).
//
// Current chain order (post-IR; the IR itself is handled in es8388_test.cpp):
//   EQ (3-band) -> Dynamics (comp/limiter/sustain) -> Output level
//
// All control entry points are CORE 0 ONLY (called from the foreground loop that
// also runs dsp_chain_process). No cross-core access — no locks needed.
#ifndef TT_DSP_CHAIN_H
#define TT_DSP_CHAIN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;     // short token used by the UART/menu (e.g. "mid_gain")
    float       value;
    float       vmin, vmax, vstep;
    const char *unit;     // "dB", "Hz", ":1", "ms", "x" — for menu display
} Param;

typedef struct Stage Stage;
struct Stage {
    const char *name;        // stage token (e.g. "eq", "comp", "out")
    bool        enabled;     // per-stage on/off (bypass when false)
    Param      *params;
    int         n_params;
    bool        dirty;       // params changed -> recompute() before next process()
    void      (*recompute)(Stage *self, float fs);
    void      (*process)(Stage *self, float *buf, int n);
    void       *state;       // stage-specific working state
};

// Global passthrough: when true, EQ + Dynamics are skipped (output level still
// applies, so levels stay safe). Per-stage `enabled` flags are preserved.
extern volatile bool g_dsp_bypass;

// Build the chain (EQ -> Dynamics -> Output) for the sample rate. out_level_lin
// seeds the output stage so it reproduces the legacy IR_OUTPUT_SCALE on boot.
void dsp_chain_init(float fs, float out_level_lin);

// Process one mono block of n samples in place through all enabled stages.
void dsp_chain_process(float *buf, int n);

// Effective IR state for the host loop: true only if the IR stage is enabled AND
// global bypass is off. The actual convolution runs cross-core in es8388_test.cpp.
bool dsp_chain_ir_enabled(void);

// Peak compressor gain reduction in dB (<= 0) since the last call; resets each call.
// For a live "GR meter" while dialing the comp threshold. 0 dB = not compressing.
float dsp_chain_comp_gr_db(void);

// Peak input level to the comp in dBFS since the last call; resets each call. Set
// comp.thresh relative to this to know where the stage actually engages.
float dsp_chain_comp_in_db(void);

// --- Presets: named bundles of (IR id + every stage's params/enables) ----------
int         dsp_chain_preset_count(void);
const char *dsp_chain_preset_name(int idx);
int         dsp_chain_find_preset(const char *name);   // -1 if not found
// Apply preset idx (sets every stage's params + enables, marks them dirty). Returns
// the preset's IR id so the host can switch the convolver IR; -1 on bad index.
int         dsp_chain_load_preset(int idx);

// Number of stages / accessor — for the future menu to iterate generically.
int    dsp_chain_stage_count(void);
Stage *dsp_chain_stage(int i);

// Parse + apply one command line (Core 0). Returns true if recognised. Emits a
// short reply via printf. Recognised forms:
//   "<stage>.<param> <value>"   set a parameter (clamped to [min,max])
//   "<stage> on" | "<stage> off"  enable / bypass a stage
//   "bypass on" | "bypass off"  global EQ+Dynamics passthrough
//   "dump"                       print every stage + param
//   "help"                       print usage
bool dsp_chain_command(const char *line);

#endif // TT_DSP_CHAIN_H