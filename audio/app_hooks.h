// audio/app_hooks.h — thin bridge so the menu (audio/) can drive preset + IR
// selection whose state actually lives in main.cpp: the IR table and the cross-core-
// safe IR switch (g_pending_ir). main.cpp implements these; menu.cpp calls them.
#ifndef TT_APP_HOOKS_H
#define TT_APP_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

// Presets (forward to dsp_chain, plus the cross-core IR switch the preset implies).
int         app_preset_count(void);
const char *app_preset_name(int i);
int         app_preset_current(void);     // index of the last-loaded preset
void        app_preset_load(int i);       // load preset i (params + IR), cross-core safe

// IR override — change the active IR for the current preset without touching params.
int         app_ir_count(void);
const char *app_ir_name(int i);
int         app_ir_current(void);         // active (or pending) IR index
void        app_ir_select(int i);         // switch active IR (cross-core safe)

#ifdef __cplusplus
}
#endif

#endif // TT_APP_HOOKS_H