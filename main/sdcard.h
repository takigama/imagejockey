#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT  "/sdcard"
#define SD_MAX_IMAGES   32
#define SD_MAX_NAME_LEN 64

typedef struct {
    char name[SD_MAX_NAME_LEN];         /* actual filename on the SD card -- kept simple/stable */
    char display_name[SD_MAX_NAME_LEN]; /* friendly label; empty means "use name" */
    uint64_t size_bytes;
} sd_image_t;

esp_err_t sd_mount(void);

/* Releases the FatFs mount (and the underlying SDMMC card object). Used
 * around SD passthrough mode's exit: the host may have changed anything
 * about the filesystem while it had raw sector access, so FatFs's cached
 * understanding of the volume (cluster size, FAT layout, etc., read once at
 * mount time) has to be thrown away and re-read fresh via sd_mount() again
 * rather than trusted. Not needed to *enter* passthrough -- that mode talks
 * to the card below FatFs entirely, so the existing mount can stay up. */
void sd_unmount(void);

/* The raw SDMMC card object underlying the FatFs mount -- used for direct
 * sdmmc_read_sectors()/write_sectors() access in SD passthrough mode,
 * bypassing FatFs. Valid whenever sd_mount() has succeeded and sd_unmount()
 * hasn't been called since; NULL otherwise. */
sdmmc_card_t *sd_get_card(void);

/* Lists *.iso / *.img files at the SD card root into `out` (capacity
 * `max_count`), returns how many were found. Not recursive -- Phase 1 keeps
 * image management to "files at the root of the card". */
size_t sd_list_images(sd_image_t *out, size_t max_count);

uint64_t sd_free_bytes(void);
uint64_t sd_total_bytes(void);
