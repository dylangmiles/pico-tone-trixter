// audio/menu.h — encoder-driven OLED menu over the DSP chain's Param/Stage model.
//
// Three levels: STAGES (ir/in/eq/comp/out + on-off) → PARAMS (a stage's enable +
// params) → EDIT (turn to adjust a value). Renders into the SH1106 framebuffer;
// the caller flushes. Reads/writes the live chain via dsp_chain.h, so the encoder,
// the UART, and the footswitches all drive the same state.
#ifndef TT_MENU_H
#define TT_MENU_H

#include <stdbool.h>

void menu_init(void);
void menu_open(void);      // enter the MAIN menu at the top row ("< back"), e.g. from the home screen
// Apply an encoder event: turn = detents (negative = CCW, positive = CW), click =
// button press. Returns true if the display changed (caller: menu_render + oled_flush).
bool menu_event(int turn, bool click);
void menu_render(void);   // draw the current menu into the OLED framebuffer

// True (once, consuming the flag) if the user clicked "< back" at the top of MAIN — the
// caller should show the home/splash screen instead of rendering the menu.
bool menu_take_home(void);

#endif // TT_MENU_H