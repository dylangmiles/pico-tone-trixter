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

// Home-screen GR-meter band on/off (UI setting; state lives in main.cpp). Off by default
// because its live repaints couple I2C crosstalk into the analog input on this build.
bool        app_gr_enabled(void);
void        app_gr_set(bool on);

// ES8388 input PGA (reg 0x09, both channels). Nibble 0..8 = 0..+24 dB in 3 dB steps.
// +12 dB (nibble 4) = OPA1642 op-amp daughter; +18 dB (nibble 6) = JFET source-follower
// daughter. State + the live codec write live in main.cpp. Driven by UART 'pga' + the menu.
int         app_pga_nib(void);        // current step 0..8
int         app_pga_db(void);         // current gain in dB (= nibble * 3)
void        app_pga_set_nib(int n);   // clamp 0..8, write the codec now

#ifdef __cplusplus
}
#endif

#endif // TT_APP_HOOKS_H