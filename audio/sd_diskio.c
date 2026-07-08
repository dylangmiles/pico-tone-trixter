// audio/sd_diskio.c — FatFs disk-I/O glue → bit-banged SD driver (sd_spi.c).
// Single volume (pdrv 0), read-only (FF_FS_READONLY=1), 512-byte sectors.

#include "ff.h"
#include "diskio.h"
#include "audio/sd_spi.h"

#include <stddef.h>
#include <stdint.h>

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return sd_is_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    return sd_init() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    for (UINT i = 0; i < count; i++)
        if (!sd_read_block((uint32_t)sector + i, buff + (size_t)i * 512))
            return RES_ERROR;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:        return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = sd_sector_count(); return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD  *)buff = 512;               return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1;                 return RES_OK;
        default:               return RES_PARERR;
    }
}

// Read-only build: FatFs never calls disk_write, but diskio.h declares it — provide a stub
// so the symbol resolves if anything references it.
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv; (void)buff; (void)sector; (void)count;
    return RES_WRPRT;
}
