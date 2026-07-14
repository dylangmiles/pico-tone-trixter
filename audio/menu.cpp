// audio/menu.cpp — see menu.h.

#include "audio/menu.h"
#include "audio/dsp_chain.h"
#include "audio/app_hooks.h"
#include "audio/oled.h"

#include <stdio.h>

enum { M_STAGES, M_PARAMS, M_EDIT, M_PRESET, M_IR, M_PGA };

static int s_mode  = M_STAGES;
static int s_sel   = 0;   // main-level row (0=back, 1=Preset, 2=IR, 3=GR meter, 4=PGA, 5+=stage)
static int s_stage = 0;   // entered stage (PARAMS / EDIT levels)
static int s_item  = 0;   // selected item in PARAMS: 0 = "< back", 1 = enable, 2+ = param[item-2]
static int s_pick  = 0;   // selected entry in the PRESET / IR picker
static bool s_go_home = false;  // set when "< back" clicked at MAIN; consumed by menu_take_home()

#define N_SPECIAL 5       // main-level rows before the stages: back, Preset, IR, GR meter, PGA
#define VIS_ROWS  7       // visible list rows below the title (8 text rows total, row 0 = title)

void menu_init(void) { s_mode = M_STAGES; s_sel = 0; s_stage = 0; s_item = 0; s_pick = 0; s_go_home = false; }

// Open MAIN at the top ("< back") — so a click on the home screen enters the menu, and the
// next click (on the now-selected "< back") returns to home.
void menu_open(void) { s_mode = M_STAGES; s_sel = 0; }

bool menu_take_home(void) { bool h = s_go_home; s_go_home = false; return h; }

static void clamp(int *v, int n) { if (*v < 0) *v = 0; if (*v >= n) *v = n - 1; }

// Items in a stage's PARAMS list: "< back", "enable", then each param.
static int params_count(Stage *st) { return 2 + st->n_params; }

bool menu_event(int turn, bool click) {
    int nstage = dsp_chain_stage_count();

    if (s_mode == M_STAGES) {
        int n_top = N_SPECIAL + nstage;
        if (turn) { s_sel += turn; clamp(&s_sel, n_top); }
        if (click) {
            if (s_sel == 0)      { s_go_home = true; }                // "< back" → home/splash
            else if (s_sel == 1) { s_mode = M_PRESET; s_pick = app_preset_current(); }
            else if (s_sel == 2) { s_mode = M_IR;     s_pick = app_ir_current(); }
            else if (s_sel == 3) { app_gr_set(!app_gr_enabled()); }   // toggle home GR meter in place
            else if (s_sel == 4) { s_mode = M_PGA; }                  // ES8388 input PGA gain (op-amp/JFET)
            else                 { s_mode = M_PARAMS; s_stage = s_sel - N_SPECIAL; s_item = 0; }
        }
        return turn || click;
    }

    if (s_mode == M_PRESET) {                          // pick a preset (loads params + IR)
        int n = app_preset_count();
        if (turn) { s_pick += turn; clamp(&s_pick, n); }
        if (click) { app_preset_load(s_pick); s_mode = M_STAGES; }
        return turn || click;
    }

    if (s_mode == M_IR) {                              // change the IR for the current preset
        int n = app_ir_count();
        if (turn) { s_pick += turn; clamp(&s_pick, n); }
        if (click) { app_ir_select(s_pick); s_mode = M_STAGES; }
        return turn || click;
    }

    if (s_mode == M_PGA) {                             // ES8388 input PGA gain (codec, live)
        if (turn)  app_pga_set_nib(app_pga_nib() + turn);   // ±3 dB per detent, clamped in the hook
        if (click) s_mode = M_STAGES;
        return turn || click;
    }

    Stage *st = dsp_chain_stage(s_stage);
    if (!st) { s_mode = M_STAGES; return true; }

    if (s_mode == M_PARAMS) {
        int nit = params_count(st);
        if (turn) { s_item += turn; clamp(&s_item, nit); }
        if (click) {
            if (s_item == 0)      s_mode = M_STAGES;            // back
            else if (s_item == 1) st->enabled = !st->enabled;  // toggle the stage
            else                  s_mode = M_EDIT;             // edit param[s_item-2]
        }
        return turn || click;
    }

    // M_EDIT
    if (s_item >= 2 && s_item - 2 < st->n_params) {
        Param *p = &st->params[s_item - 2];
        if (turn) {
            p->value += (float)turn * p->vstep;
            if (p->value < p->vmin) p->value = p->vmin;
            if (p->value > p->vmax) p->value = p->vmax;
            st->dirty = true;                                  // recompute coeffs next block
        }
    }
    if (click) s_mode = M_PARAMS;
    return turn || click;
}

// Top index so that `sel` is within the visible window.
static int scroll_top(int sel, int n, int vis) {
    int top = (sel >= vis) ? sel - vis + 1 : 0;
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;
    return top;
}

static void row(int i, int sel, const char *s) {
    int y = 8 + i * 8;
    if (i == sel) oled_text_inv(0, y, s);   // (sel passed already window-relative)
    else          oled_text(0, y, s);
}

// Draw a single-column picker list (preset / IR), marking the active entry with '*'.
static void picker(const char *title, int n, int sel,
                   const char *(*name)(int), int active) {
    oled_text(0, 0, title);
    int top = scroll_top(sel, n, VIS_ROWS);
    char line[24];
    for (int i = 0; i < VIS_ROWS && top + i < n; i++) {
        int it = top + i;
        snprintf(line, sizeof line, "%-12s%s", name(it), it == active ? "*" : "");
        row(i, sel - top, line);
    }
}

void menu_render(void) {
    oled_clear();
    char line[24];

    if (s_mode == M_STAGES) {
        oled_text(0, 0, "-- MAIN --");
        int n_top = N_SPECIAL + dsp_chain_stage_count();
        int top = scroll_top(s_sel, n_top, VIS_ROWS);
        for (int i = 0; i < VIS_ROWS && top + i < n_top; i++) {
            int it = top + i;
            if (it == 0)      snprintf(line, sizeof line, "< back");
            else if (it == 1) snprintf(line, sizeof line, "P: %s",  app_preset_name(app_preset_current()));
            else if (it == 2) snprintf(line, sizeof line, "IR:%s",  app_ir_name(app_ir_current()));
            else if (it == 3) snprintf(line, sizeof line, "GR meter  %s", app_gr_enabled() ? "on" : "off");
            else if (it == 4) snprintf(line, sizeof line, "%-8s  +%d dB", "PGA", app_pga_db());
            else {
                Stage *st = dsp_chain_stage(it - N_SPECIAL);
                // value column at char 10, aligned with the "GR meter  <on/off>" row above
                snprintf(line, sizeof line, "%-8s  %s", st->name, st->enabled ? "on" : "off");
            }
            row(i, s_sel - top, line);
        }
    } else if (s_mode == M_PRESET) {
        picker("-- PRESET --", app_preset_count(), s_pick, app_preset_name, app_preset_current());
    } else if (s_mode == M_IR) {
        picker("-- IR --", app_ir_count(), s_pick, app_ir_name, app_ir_current());
    } else if (s_mode == M_PGA) {
        oled_text(0, 0, "-- PGA --");
        snprintf(line, sizeof line, "+%d dB", app_pga_db());
        oled_text(0, 26, line);
        oled_text(0, 42, "12=opamp 18=jfet");
        oled_text(0, 56, "turn=adj click=ok");
    } else if (s_mode == M_PARAMS) {
        Stage *st = dsp_chain_stage(s_stage);
        snprintf(line, sizeof line, "-- %s --", st->name);
        oled_text(0, 0, line);
        int nit = params_count(st);
        int top = scroll_top(s_item, nit, VIS_ROWS);
        for (int i = 0; i < VIS_ROWS && top + i < nit; i++) {
            int it = top + i;
            if (it == 0)      snprintf(line, sizeof line, "< back");
            else if (it == 1) snprintf(line, sizeof line, "enable    %s", st->enabled ? "on" : "off");
            else {
                Param *p = &st->params[it - 2];
                snprintf(line, sizeof line, "%-7s %6.2f%s", p->name, (double)p->value, p->unit);
            }
            row(i, s_item - top, line);
        }
    } else { // M_EDIT
        Stage *st = dsp_chain_stage(s_stage);
        Param *p  = &st->params[s_item - 2];
        snprintf(line, sizeof line, "%s.%s", st->name, p->name);
        oled_text(0, 0, line);
        snprintf(line, sizeof line, "%.2f %s", (double)p->value, p->unit);
        oled_text(0, 26, line);
        snprintf(line, sizeof line, "[%.2f..%.2f]", (double)p->vmin, (double)p->vmax);
        oled_text(0, 42, line);
        oled_text(0, 56, "turn=adj click=ok");
    }
}