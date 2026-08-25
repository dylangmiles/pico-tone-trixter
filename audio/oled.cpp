// audio/oled.cpp — SH1106 128×64 OLED driver (I2C1 @ 0x3C). See oled.h.
//
// SH1106 differs from SSD1306: DC-DC control is 0xAD/0x8B (not charge-pump 0x8D),
// it has a pump-voltage command (0x32), and the 132-col RAM means the 128-col panel
// starts at column offset 2 — handled in oled_flush().

#include "audio/oled.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include <string.h>
#include <stdio.h>

#define OLED_ADDR   0x3C
#define OLED_W      128
#define OLED_H      64
#define OLED_PAGES  (OLED_H / 8)        // 8
#define OLED_COLOFF 2                   // SH1106 132-col RAM → 128-col panel offset

static uint8_t s_fb[OLED_W * OLED_PAGES];   // 1024-byte framebuffer (page-major, 1 byte = 8 vertical px)

// 5×7 font, printable ASCII 0x20–0x7E. 5 column-bytes per glyph, bit0 = top row.
static const uint8_t FONT5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x00,0x00,0x5f,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7f,0x14,0x7f,0x14}, // #
    {0x24,0x2a,0x7f,0x2a,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1c,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1c,0x00}, // )
    {0x14,0x08,0x3e,0x08,0x14}, // *
    {0x08,0x08,0x3e,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3e,0x51,0x49,0x45,0x3e}, // 0
    {0x00,0x42,0x7f,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4b,0x31}, // 3
    {0x18,0x14,0x12,0x7f,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3c,0x4a,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1e}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3e}, // @
    {0x7e,0x11,0x11,0x11,0x7e}, // A
    {0x7f,0x49,0x49,0x49,0x36}, // B
    {0x3e,0x41,0x41,0x41,0x22}, // C
    {0x7f,0x41,0x41,0x22,0x1c}, // D
    {0x7f,0x49,0x49,0x49,0x41}, // E
    {0x7f,0x09,0x09,0x09,0x01}, // F
    {0x3e,0x41,0x49,0x49,0x7a}, // G
    {0x7f,0x08,0x08,0x08,0x7f}, // H
    {0x00,0x41,0x7f,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3f,0x01}, // J
    {0x7f,0x08,0x14,0x22,0x41}, // K
    {0x7f,0x40,0x40,0x40,0x40}, // L
    {0x7f,0x02,0x0c,0x02,0x7f}, // M
    {0x7f,0x04,0x08,0x10,0x7f}, // N
    {0x3e,0x41,0x41,0x41,0x3e}, // O
    {0x7f,0x09,0x09,0x09,0x06}, // P
    {0x3e,0x41,0x51,0x21,0x5e}, // Q
    {0x7f,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7f,0x01,0x01}, // T
    {0x3f,0x40,0x40,0x40,0x3f}, // U
    {0x1f,0x20,0x40,0x20,0x1f}, // V
    {0x3f,0x40,0x38,0x40,0x3f}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7f,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x41,0x7f,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7f,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7f}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7e,0x09,0x01,0x02}, // f
    {0x0c,0x52,0x52,0x52,0x3e}, // g
    {0x7f,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7d,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3d,0x00}, // j
    {0x7f,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7f,0x40,0x00}, // l
    {0x7c,0x04,0x18,0x04,0x78}, // m
    {0x7c,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7c,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7c}, // q
    {0x7c,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3f,0x44,0x40,0x20}, // t
    {0x3c,0x40,0x40,0x20,0x7c}, // u
    {0x1c,0x20,0x40,0x20,0x1c}, // v
    {0x3c,0x40,0x30,0x40,0x3c}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0c,0x50,0x50,0x50,0x3c}, // y
    {0x44,0x64,0x54,0x4c,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7f,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x08,0x04,0x08,0x10,0x08}, // ~
};

static void oled_cmds(const uint8_t *cmds, int n) {
    uint8_t buf[40];
    buf[0] = 0x00;                  // Co=0, D/C#=0 → command stream
    memcpy(buf + 1, cmds, n);
    i2c_write_blocking(i2c1, OLED_ADDR, buf, n + 1, false);
}

// OLED hardware reset pin. THE cure for flaky power-on init (garbage / vertical offset /
// black screen) and the only way a software re-init can actually recover a latched panel.
// Needs the panel's RES wire moved OFF VCC onto this free GPIO (GP11 — left side, well
// clear of the audio clocks on GP16/17/21). Safe to leave set even before the rewire: it
// just pulses an unconnected pin, and the POR settle still runs. Set to -1 to disable.
#define OLED_RES_PIN  11

static const uint8_t OLED_INIT[] = {
    0xAE,             // display OFF (stays off until RAM is cleared)
    0xD5, 0x80,       // clock divide / osc freq
    0xA8, 0x3F,       // multiplex ratio = 64
    0xD3, 0x00,       // display offset = 0   <-- if this byte's arg is dropped, image shifts
    0x40,             // start line = 0       <-- ditto: vertical offset symptom
    0xAD, 0x8B,       // SH1106 DC-DC control: on
    0xA1,             // segment remap (col 127 → SEG0)
    0xC8,             // COM scan direction remapped
    0xDA, 0x12,       // COM pins config
    0x81, 0x80,       // contrast
    0xD9, 0x22,       // pre-charge
    0xDB, 0x35,       // VCOMH deselect
    0x32,             // SH1106 pump voltage = 8.0 V
    0xA4,             // resume to RAM content
    0xA6,             // normal (non-inverted)
};                    // NB: no 0xAF — display turned on AFTER the clear below

bool oled_init(void) {
#if OLED_RES_PIN >= 0
    // Hardware reset: forces the SH1106 controller into a known state regardless of how
    // VCC ramped. THE fix for the intermittent bad init / black screen — and the only way
    // a software re-init (double-push / `oled`) can actually recover a latched panel.
    gpio_init(OLED_RES_PIN);
    gpio_set_dir(OLED_RES_PIN, GPIO_OUT);
    gpio_put(OLED_RES_PIN, 1); sleep_ms(1);
    gpio_put(OLED_RES_PIN, 0); sleep_ms(10);   // assert reset (>= a few µs needed; 10 ms is ample)
    gpio_put(OLED_RES_PIN, 1);                 // release
#endif
    sleep_ms(120);                 // POR / charge-pump settle (always — harmless with HW reset too)

    // Probe with retries — a marginal-VCC panel can be slow to start ACKing after power.
    // Give it up to ~8 tries (~160 ms) before declaring it absent.
    uint8_t rx;
    int tries = 0;
    while (i2c_read_blocking(i2c1, OLED_ADDR, &rx, 1, false) < 0) {
        if (++tries >= 8) return false;
        sleep_ms(20);
    }

    // UN-STICK A LATCHED CONTROLLER before configuring it. Without a hardware RES, a
    // panel interrupted mid-command sits waiting for a PARAMETER byte -- and then eats
    // the first byte of our init sequence as that parameter, shifting everything after
    // it. That misalignment IS the vertically-offset splash. Double-sending the config
    // cannot fix it: the second pass just starts from a different misalignment.
    //
    // 0xE3 is NOP and takes no parameters. A short run satisfies any pending parameter
    // (harmlessly, as a value) and leaves the controller aligned in command state, so the
    // real sequence below lands on a receptive part. Then 0xAE blanks it, so a re-init of
    // a live garbled panel does not show junk while it is being reconfigured.
    // Costs microseconds. Only genuinely needed while RES is unwired -- but harmless with
    // a hardware reset too, so it stays either way.
    static const uint8_t unstick[] = { 0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xE3, 0xAE };
    oled_cmds(unstick, sizeof unstick);
    sleep_ms(2);

    // Send the config TWICE (register writes are idempotent). Without a hardware reset,
    // the first I2C burst right after power-up occasionally drops a byte — which misaligns
    // the rest of the sequence and shows as a vertically offset image (splash starting
    // half-way down / near the bottom). The second pass re-asserts the correct config.
    oled_cmds(OLED_INIT, sizeof OLED_INIT);
    sleep_ms(5);
    oled_cmds(OLED_INIT, sizeof OLED_INIT);
    sleep_ms(20);                  // DC-DC pump stabilise before first RAM write

    oled_clear();
    oled_flush();                  // RAM now blank...

    static const uint8_t on = 0xAF;
    oled_cmds(&on, 1);             // ...so the display lights up clean, never garbage
    return true;
}

void oled_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

void oled_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    uint8_t *b = &s_fb[(y / 8) * OLED_W + x];
    uint8_t  m = 1u << (y & 7);
    if (on) *b |= m; else *b &= ~m;
}

// --- Audio-safe asynchronous DMA flush --------------------------------------
// SH1106 has no cross-page auto-increment, so the frame can't be one DMA burst: each page
// needs a 4-byte page/col command before its 128 data bytes. We push it page-at-a-time,
// serviced from the Core 0 foreground loop. The 4-byte command stays a blocking i2c write
// (~0.7 ms, negligible); the 129-byte data payload (0x40 control + 128 px) goes out via DMA
// paced by the I2C TX DREQ — Core 0 is free to convolve during those ~23 ms/page. i2c1 is
// otherwise idle at runtime (ES8388 is only touched at init), so no bus contention. The 50 kHz
// clock is kept: it's chosen to keep the I2C edges out of the audio (EMI), not for timing —
// and with DMA its transfer time no longer costs any Core 0 stall.
static int      s_dma_chan   = -1;
static volatile bool s_flush_running = false;    // a page range is being pushed
static bool     s_dma_inflight = false;          // the current page's data DMA is feeding the FIFO
static int      s_flush_page  = 0;               // next page to push
static int      s_flush_last  = 0;               // last page in the active range
static int      s_pending_first = -1;            // a range requested while one was in flight
static int      s_pending_last  = -1;            // (coalesced into the widest cover)
// 0x40 ctrl + 128 data, as full 32-bit IC_DATA_CMD words. 32-bit (not 16-bit) matches how
// the SDK itself writes data_cmd (io_rw_32) — safest access width for the APB register.
static uint32_t s_dma_words[1 + OLED_W];

static void oled_dma_claim(void) {
    if (s_dma_chan < 0) {
        s_dma_chan = dma_claim_unused_channel(true);
        // Enable the I2C's TX DMA request line. Without TDMAE the DW_apb_i2c never asserts
        // its TX DREQ, so a DREQ-paced DMA to data_cmd stalls forever (transfers nothing).
        // The SDK's blocking i2c API doesn't set this. Harmless to leave on for blocking
        // writes — the DREQ is simply ignored when no DMA is listening.
        i2c1->hw->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS;
    }
}

// The previous page's data has fully left i2c1 (FIFO drained + STOP shifted out), so a new
// transaction can safely start. Checked only after the DMA has finished feeding the FIFO.
static inline bool oled_i2c_tx_done(void) {
    return i2c1->hw->txflr == 0 &&
           !(i2c1->hw->status & I2C_IC_STATUS_MST_ACTIVITY_BITS);
}

void oled_flush_service(void) {
    if (!s_flush_running) return;

    if (s_dma_inflight) {
        if (dma_channel_is_busy(s_dma_chan)) return;   // DMA still feeding the 16-deep TX FIFO
        if (!oled_i2c_tx_done()) return;               // FIFO still draining / STOP not out yet
        s_dma_inflight = false;
        s_flush_page++;                                // this page is on the panel
    }

    if (s_flush_page > s_flush_last) {                 // range done — start a queued one, if any
        if (s_pending_first >= 0) {
            s_flush_page = s_pending_first;
            s_flush_last = s_pending_last;
            s_pending_first = s_pending_last = -1;
        } else {
            s_flush_running = false;
            return;
        }
    }

    // Set the page/column pointer (tiny blocking write; leaves i2c1 tar = OLED_ADDR).
    int page = s_flush_page;
    uint8_t set[] = { 0x00,                             // Co=0, D/C#=0 → command stream
                      (uint8_t)(0xB0 | page),
                      (uint8_t)(0x00 | (OLED_COLOFF & 0x0F)),
                      (uint8_t)(0x10 | (OLED_COLOFF >> 4)) };
    i2c_write_blocking(i2c1, OLED_ADDR, set, sizeof(set), false);

    // Build the data payload as IC_DATA_CMD words and DMA it out; STOP on the final byte.
    s_dma_words[0] = 0x40;                             // Co=0, D/C#=1 → data stream
    const uint8_t *src = &s_fb[page * OLED_W];
    for (int i = 0; i < OLED_W; i++) s_dma_words[1 + i] = src[i];
    s_dma_words[OLED_W] |= I2C_IC_DATA_CMD_STOP_BITS;  // STOP after last pixel byte

    dma_channel_config c = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, i2c_get_dreq(i2c1, true));        // pace on I2C TX FIFO space
    dma_channel_configure(s_dma_chan, &c, &i2c1->hw->data_cmd, s_dma_words, 1 + OLED_W, true);
    s_dma_inflight = true;
}

bool oled_flush_busy(void) { return s_flush_running; }

void oled_flush_pages_async(int first, int last) {
    if (first < 0) first = 0;
    if (last > OLED_PAGES - 1) last = OLED_PAGES - 1;
    if (first > last) return;
    oled_dma_claim();
    if (s_flush_running) {                              // coalesce: widen the queued range
        if (s_pending_first < 0) { s_pending_first = first; s_pending_last = last; }
        else {
            if (first < s_pending_first) s_pending_first = first;
            if (last  > s_pending_last)  s_pending_last  = last;
        }
        return;
    }
    s_flush_page   = first;
    s_flush_last   = last;
    s_dma_inflight = false;
    s_flush_running = true;
    oled_flush_service();                               // kick the first page now
}

void oled_flush_async(void) { oled_flush_pages_async(0, OLED_PAGES - 1); }

void oled_dma_selftest(void) {
    oled_dma_claim();
    printf("[oleddma] chan=%d dma_cr=0x%08x TDMAE=%d dreq=%d addr=%p\n",
           s_dma_chan, (unsigned)i2c1->hw->dma_cr,
           (int)((i2c1->hw->dma_cr & I2C_IC_DMA_CR_TDMAE_BITS) ? 1 : 0),
           (int)i2c_get_dreq(i2c1, true), (void *)&i2c1->hw->data_cmd);

    // Obvious test pattern: full border + text. If this shows, the async DMA path works.
    oled_clear();
    for (int x = 0; x < OLED_W; x++) { oled_pixel(x, 0, true); oled_pixel(x, OLED_H - 1, true); }
    for (int y = 0; y < OLED_H; y++) { oled_pixel(0, y, true); oled_pixel(OLED_W - 1, y, true); }
    oled_text(8, 8,  "DMA SELFTEST");
    oled_text(8, 24, "async flush ok");

    absolute_time_t t0 = get_absolute_time();
    oled_flush_async();
    int max_page = -1, iters = 0;
    while (s_flush_running) {
        oled_flush_service();
        iters++;
        if (s_flush_page > max_page) max_page = s_flush_page;
        if (absolute_time_diff_us(t0, get_absolute_time()) > 1000000) {   // 1 s stall cap
            printf("[oleddma] STALL page=%d inflight=%d dma_busy=%d remaining=%u "
                   "txflr=%u mst_active=%d iters=%d\n",
                   s_flush_page, (int)s_dma_inflight,
                   (int)dma_channel_is_busy(s_dma_chan),
                   (unsigned)dma_channel_hw_addr(s_dma_chan)->transfer_count,
                   (unsigned)i2c1->hw->txflr,
                   (int)((i2c1->hw->status & I2C_IC_STATUS_MST_ACTIVITY_BITS) ? 1 : 0),
                   iters);
            dma_channel_abort(s_dma_chan);            // don't leave the state machine wedged
            s_dma_inflight = false;
            s_flush_running = false;
            return;
        }
    }
    printf("[oleddma] OK completed in %lld us, max_page=%d iters=%d\n",
           (long long)absolute_time_diff_us(t0, get_absolute_time()), max_page, iters);
}

// Finish any in-flight async flush before a blocking transfer touches the same bus.
static void oled_flush_drain(void) {
    while (s_flush_running) oled_flush_service();
}

void oled_flush_pages(int first_page, int last_page) {
    oled_flush_drain();                                 // don't collide with an async flush
    if (first_page < 0) first_page = 0;
    if (last_page > OLED_PAGES - 1) last_page = OLED_PAGES - 1;
    for (int page = first_page; page <= last_page; page++) {
        uint8_t set[] = { (uint8_t)(0xB0 | page),
                          (uint8_t)(0x00 | (OLED_COLOFF & 0x0F)),
                          (uint8_t)(0x10 | (OLED_COLOFF >> 4)) };
        oled_cmds(set, sizeof(set));
        uint8_t row[1 + OLED_W];
        row[0] = 0x40;                              // Co=0, D/C#=1 → data stream
        memcpy(row + 1, &s_fb[page * OLED_W], OLED_W);
        i2c_write_blocking(i2c1, OLED_ADDR, row, sizeof(row), false);
    }
}

void oled_flush(void) { oled_flush_pages(0, OLED_PAGES - 1); }

void oled_fill_rect(int x, int y, int w, int h, bool on) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            oled_pixel(xx, yy, on);
}

static void draw_char(int x, int y, char c, bool inv) {
    if (c < 0x20 || c > 0x7E) c = 0x20;
    const uint8_t *g = FONT5x7[c - 0x20];
    for (int col = 0; col < 6; col++) {
        uint8_t bits = (col < 5) ? g[col] : 0x00;  // 6th column = spacing
        for (int row = 0; row < 8; row++) {
            bool on = (bits >> row) & 1;
            oled_pixel(x + col, y + row, inv ? !on : on);
        }
    }
}

void oled_text(int x, int y, const char *s) {
    for (; *s; s++) { draw_char(x, y, *s, false); x += 6; }
}

// 2× scaled glyph: each font pixel drawn as a 2×2 block (12×16 cell). Set pixels only,
// so it overlays cleanly. Used for the big tuner note.
static void draw_char2x(int x, int y, char c) {
    if (c < 0x20 || c > 0x7E) c = 0x20;
    const uint8_t *g = FONT5x7[c - 0x20];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
            if ((g[col] >> row) & 1)
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        oled_pixel(x + col * 2 + dx, y + row * 2 + dy, true);
}

void oled_text2x(int x, int y, const char *s) {
    for (; *s; s++) { draw_char2x(x, y, *s); x += 12; }
}

void oled_text_inv(int x, int y, const char *s) {
    for (; *s; s++) { draw_char(x, y, *s, true); x += 6; }
}