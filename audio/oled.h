// audio/oled.h — SH1106 128×64 monochrome OLED over I2C1 @ 0x3C.
//
// Shares the ES8388 I2C bus (i2c1, GP14 SDA / GP15 SCL), so i2c1 must already be
// initialised (it is, by the ES8388 setup) before oled_init(). 6×8 text (5×7 glyphs
// in a 6-px cell). Draw into the framebuffer, then oled_flush() to show it.
#ifndef TT_OLED_H
#define TT_OLED_H

#include <stdbool.h>

bool oled_init(void);                          // false if the panel doesn't ACK on the bus
void oled_clear(void);                         // clear the framebuffer (call oled_flush to show)
void oled_flush(void);                         // push the framebuffer to the panel
void oled_pixel(int x, int y, bool on);
void oled_text(int x, int y, const char *s);   // text, top-left pixel (x,y)
void oled_text_inv(int x, int y, const char *s); // inverted (for a selected menu row)

#endif // TT_OLED_H