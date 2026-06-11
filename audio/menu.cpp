// audio/menu.cpp — see menu.h.

#include "audio/menu.h"
#include "audio/dsp_chain.h"
#include "audio/oled.h"

#include <stdio.h>

enum { M_STAGES, M_PARAMS, M_EDIT };

static int s_mode  = M_STAGES;
static int s_sel   = 0;   // selected stage index (STAGES level)
static int s_stage = 0;   // entered stage (PARAMS / EDIT levels)
static int s_item  = 0;   // selected item in PARAMS: 0 = "< back", 1 = enable, 2+ = param[item-2]

#define VIS_ROWS 7        // visible list rows below the title (8 text rows total, row 0 = title)

void menu_init(void) { s_mode = M_STAGES; s_sel = 0; s_stage = 0; s_item = 0; }

static void clamp(int *v, int n) { if (*v < 0) *v = 0; if (*v >= n) *v = n - 1; }

// Items in a stage's PARAMS list: "< back", "enable", then each param.
static int params_count(Stage *st) { return 2 + st->n_params; }

bool menu_event(int turn, bool click) {
    int nstage = dsp_chain_stage_count();

    if (s_mode == M_STAGES) {
        if (turn) { s_sel += turn; clamp(&s_sel, nstage); }
        if (click) { s_mode = M_PARAMS; s_stage = s_sel; s_item = 0; }
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
    if (i + 0 == sel) oled_text_inv(0, y, s);   // (sel passed already window-relative)
    else              oled_text(0, y, s);
}

void menu_render(void) {
    oled_clear();
    char line[24];

    if (s_mode == M_STAGES) {
        oled_text(0, 0, "-- STAGES --");
        int n = dsp_chain_stage_count();
        int top = scroll_top(s_sel, n, VIS_ROWS);
        for (int i = 0; i < VIS_ROWS && top + i < n; i++) {
            Stage *st = dsp_chain_stage(top + i);
            snprintf(line, sizeof line, "%-5s   %s", st->name, st->enabled ? "on" : "off");
            row(i, s_sel - top, line);
        }
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