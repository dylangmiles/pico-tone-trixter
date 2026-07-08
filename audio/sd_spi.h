// audio/sd_spi.h — SD card over BIT-BANGED SPI on GP6/8/9/10.
//
// GP6/8/9/10 can't form an RP2350 hardware-SPI instance (see project_sd_card_pinout), so
// this is a software SPI in SPI mode 0. Pins match the proto-board SD header (col 13,
// rows 8-15): MISO=GP6, VCC=GP7(cut strip), CS=GP8, SCK=GP9, GND=r12, MOSI=GP10.
//
// Read-only: enough to init a card and read 512-byte sectors, which is all FatFs needs to
// load IR / preset files. Init runs at ~250 kHz (SD spec caps identify mode at 400 kHz);
// data transfer then runs at the bit-bang's native speed. Not audio-safe — reads block the
// caller, so only load from SD at deliberate, glitch-tolerant moments (preset/IR switch).
#ifndef TT_SD_SPI_H
#define TT_SD_SPI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MISO_PIN   6
#define SD_CS_PIN     8
#define SD_SCK_PIN    9
#define SD_MOSI_PIN  10

bool     sd_init(void);                          // power-up + SPI-mode init; false if no card
bool     sd_read_block(uint32_t lba, uint8_t *dst);   // read one 512-byte sector (LBA)
uint32_t sd_sector_count(void);                  // total 512-byte sectors (from CSD), 0 if unknown
bool     sd_is_ready(void);                      // initialised OK
bool     sd_is_sdhc(void);                       // block-addressed (SDHC/SDXC) vs byte (SDSC)
void     sd_set_verbose(bool v);                 // print a CMD0/CMD8/ACMD41 init trace
void     sd_pin_check(void);                      // DISCONNECT module first: float/short test on GP6/8/9/10

#ifdef __cplusplus
}
#endif

#endif // TT_SD_SPI_H
