// audio/tt_store.cpp — see tt_store.h. Flow-YAML parser for config + presets.
#include "audio/tt_store.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdlib.h>

extern "C" {
#include "ff.h"
}

#define MAX_SD_PRESETS 12
#define NAME_MAX       40
#define IR_MAX         64

// Parsed results (static storage — dsp_chain_install_presets keeps the pointer).
static Preset s_sd[MAX_SD_PRESETS];
static char   s_name[MAX_SD_PRESETS][NAME_MAX];
static char   s_ir[MAX_SD_PRESETS][IR_MAX];
static int    s_n = 0;

static char   s_boot[NAME_MAX] = "";
static bool   s_gr = false, s_gr_set = false;
static bool   s_have_config = false;

// --- small text helpers ----------------------------------------------------
// Trim leading/trailing ASCII whitespace in place; return the trimmed start.
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

// Strip surrounding single/double quotes in place.
static char *unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') || (s[0] == '\'' && s[n-1] == '\''))) {
        s[n-1] = 0;
        return s + 1;
    }
    return s;
}

static bool parse_bool(const char *v) {
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' ||
           strcmp(v, "on") == 0 || strcmp(v, "ON") == 0 || strcmp(v, "On") == 0;
}

// Parse an inline flow list "[a, b, c]" (brackets optional) into out[]; returns count.
static int parse_list(const char *v, float *out, int maxn) {
    const char *s = strchr(v, '[');
    s = s ? s + 1 : v;
    int n = 0;
    while (n < maxn) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (*s == ']' || *s == 0) break;
        char *end;
        float f = strtof(s, &end);
        if (end == s) break;
        out[n++] = f;
        s = end;
    }
    return n;
}

// Read a whole small file into buf (NUL-terminated). Returns length, or -1 if absent.
static int read_file(const char *path, char *buf, int cap) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    UINT total = 0, br;
    while (total < (UINT)(cap - 1)) {
        if (f_read(&f, buf + total, (UINT)(cap - 1) - total, &br) != FR_OK) { f_close(&f); return -1; }
        if (br == 0) break;
        total += br;
    }
    buf[total] = 0;
    f_close(&f);
    return (int)total;
}

// Split "key: value" in place. Returns false if there's no ':'. Both sides trimmed.
static bool split_kv(char *line, char **key, char **val) {
    char *colon = strchr(line, ':');
    if (!colon) return false;
    *colon = 0;
    *key = trim(line);
    *val = trim(colon + 1);
    return true;
}

// --- config.txt ------------------------------------------------------------
static void parse_config(char *buf) {
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *hash = strchr(p, '#');
        if (hash) *hash = 0;
        char *line = trim(p);
        char *k, *v;
        if (*line && split_kv(line, &k, &v)) {
            if (strcmp(k, "boot_preset") == 0) {
                strncpy(s_boot, unquote(v), NAME_MAX - 1); s_boot[NAME_MAX - 1] = 0;
                s_have_config = true;
            } else if (strcmp(k, "gr_meter") == 0) {
                s_gr = parse_bool(v); s_gr_set = true; s_have_config = true;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

// --- presets.txt -----------------------------------------------------------
static void commit(const Preset *cur, const char *nm, const char *ir, bool *active) {
    if (!*active) return;
    *active = false;
    if (nm[0] == 0 || s_n >= MAX_SD_PRESETS) return;
    strncpy(s_name[s_n], nm, NAME_MAX - 1); s_name[s_n][NAME_MAX - 1] = 0;
    strncpy(s_ir[s_n],   ir, IR_MAX - 1);   s_ir[s_n][IR_MAX - 1]     = 0;
    s_sd[s_n]      = *cur;
    s_sd[s_n].name = s_name[s_n];
    s_sd[s_n].ir   = s_ir[s_n][0] ? s_ir[s_n] : NULL;
    s_n++;
}

static void parse_presets(char *buf) {
    const Preset *def = dsp_chain_default_preset();
    Preset cur = *def;
    char nm[NAME_MAX] = "";
    char ir[IR_MAX];
    strncpy(ir, def->ir ? def->ir : "", IR_MAX - 1); ir[IR_MAX - 1] = 0;
    bool active = false;

    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *hash = strchr(p, '#');
        if (hash) *hash = 0;
        char *line = trim(p);
        if (!*line) { if (!nl) break; p = nl + 1; continue; }

        if (strcmp(line, "---") == 0) {
            commit(&cur, nm, ir, &active);
            cur = *def; nm[0] = 0;
            strncpy(ir, def->ir ? def->ir : "", IR_MAX - 1); ir[IR_MAX - 1] = 0;
            if (!nl) break; p = nl + 1; continue;
        }

        char *k, *v;
        if (split_kv(line, &k, &v)) {
            if (strcmp(k, "name") == 0) {
                commit(&cur, nm, ir, &active);            // implicit separator if --- omitted
                cur = *def;
                strncpy(ir, def->ir ? def->ir : "", IR_MAX - 1); ir[IR_MAX - 1] = 0;
                strncpy(nm, unquote(v), NAME_MAX - 1); nm[NAME_MAX - 1] = 0;
                active = true;
            } else if (active) {
                float a[5];
                if (strcmp(k, "ir") == 0) {          // "none"/"off"/empty → no IR (convolution off)
                    const char *uv = unquote(v);
                    if (uv[0] == 0 || strcasecmp(uv, "none") == 0 || strcasecmp(uv, "off") == 0)
                        ir[0] = 0;                   // committed as ir=NULL ⇒ resolves to the "none" IR
                    else { strncpy(ir, uv, IR_MAX - 1); ir[IR_MAX - 1] = 0; }
                }
                else if (strcmp(k, "in.on") == 0)     cur.in_on   = parse_bool(v);
                else if (strcmp(k, "eq.on") == 0)     cur.eq_on   = parse_bool(v);
                else if (strcmp(k, "comp.on") == 0)   cur.comp_on = parse_bool(v);
                else if (strcmp(k, "out.on") == 0)    cur.out_on  = parse_bool(v);
                else if (strcmp(k, "in.level") == 0)  cur.in_level  = strtof(v, NULL);
                else if (strcmp(k, "out.level") == 0) cur.out_level = strtof(v, NULL);
                else if (strcmp(k, "pga") == 0)       cur.pga = (int)strtol(v, NULL, 10);   // ES8388 PGA dB (K&K 12, Garrison 6)
                else if (strcmp(k, "eq.lo") == 0)  { int c = parse_list(v, a, 2); if (c>=1) cur.lo_f=a[0]; if (c>=2) cur.lo_g=a[1]; }
                else if (strcmp(k, "eq.mid") == 0) { int c = parse_list(v, a, 3); if (c>=1) cur.mid_f=a[0]; if (c>=2) cur.mid_g=a[1]; if (c>=3) cur.mid_q=a[2]; }
                else if (strcmp(k, "eq.hi") == 0)  { int c = parse_list(v, a, 2); if (c>=1) cur.hi_f=a[0]; if (c>=2) cur.hi_g=a[1]; }
                else if (strcmp(k, "comp") == 0)   { int c = parse_list(v, a, 5);
                    if (c>=1) cur.thr=a[0]; if (c>=2) cur.ratio=a[1]; if (c>=3) cur.att=a[2];
                    if (c>=4) cur.rel=a[3]; if (c>=5) cur.mkup=a[4]; }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    commit(&cur, nm, ir, &active);
}

// --- public API ------------------------------------------------------------
bool tt_store_load(void) {
    s_n = 0; s_have_config = false; s_gr = false; s_gr_set = false; s_boot[0] = 0;

    static char buf[8192];
    if (read_file("/tonetrix/config.txt",  buf, sizeof buf) >= 0) parse_config(buf);
    if (read_file("/tonetrix/presets.txt", buf, sizeof buf) >= 0) parse_presets(buf);

    return s_have_config || s_n > 0;
}

const Preset *tt_store_presets(int *n_out) {
    if (n_out) *n_out = s_n;
    return s_n > 0 ? s_sd : NULL;
}

const char *tt_store_boot_preset(void) { return s_boot; }

bool tt_store_gr_meter(bool *was_set) {
    if (was_set) *was_set = s_gr_set;
    return s_gr;
}

void tt_store_dump(void) {
    printf("sdcfg: config=%s", s_have_config ? "yes" : "no");
    if (s_boot[0])  printf(" boot_preset=%s", s_boot);
    if (s_gr_set)   printf(" gr_meter=%s", s_gr ? "on" : "off");
    printf("\nsdcfg: presets=%d%s\n", s_n, s_n ? "" : " (using built-ins)");
    for (int i = 0; i < s_n; i++)
        printf("  %-16s ir=%s\n", s_sd[i].name, s_sd[i].ir ? s_sd[i].ir : "(keep)");
}
