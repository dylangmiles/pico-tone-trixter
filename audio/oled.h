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
void oled_text2x(int x, int y, const char *s);   // 2× scale (12×16 cell), e.g. the tuner note
void oled_fill_rect(int x, int y, int w, int h, bool on);   // filled rectangle into the framebuffer
// Flush only pages [first_page..last_page] (each page = 8 px rows). A cheap partial update:
// the full oled_flush() is ~180 ms at the 50 kHz audio-safe I2C clock, so the live GR meter
// repaints just its band. first_page/last_page are clamped to [0, 7].
void oled_flush_pages(int first_page, int last_page);

// --- Audio-safe asynchronous flush (DMA-backed) ------------------------------
// The blocking oled_flush() above busy-waits the whole transfer on Core 0, which stalls
// the foreground audio loop (head convolution) → audible blips. These non-blocking variants
// DMA each page's data payload in the background instead: they return immediately, and the
// Core 0 loop keeps convolving while i2c1 shifts the bytes out. SH1106 needs a page-address
// command before every page (no cross-page auto-increment), so the transfer is serviced one
// page at a time — drive it by calling oled_flush_service() every foreground iteration.
//
// Use these for live/animated content (menu navigation, GR meter). Keep the blocking
// oled_flush()/oled_cmds() for boot + one-off events where a brief blip is harmless (audio
// muted at boot); those transparently drain any in-flight async flush first, so mixing is safe.
void oled_flush_async(void);                       // whole frame, non-blocking
void oled_flush_pages_async(int first, int last);  // partial, non-blocking
void oled_flush_service(void);                     // advance the async transfer; call each fg loop
bool oled_flush_busy(void);                        // true while an async flush is in flight/queued

// Diagnostic: draw a test pattern, run one async flush to completion with a 1 s cap, and
// printf the DMA/I2C state (TDMAE, DREQ, per-page progress, or exactly where it stalled).
// Isolates the DMA path from the audio loop. Wired to the `oleddma` UART command.
void oled_dma_selftest(void);

#endif // TT_OLED_H