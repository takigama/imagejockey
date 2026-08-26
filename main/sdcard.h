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
 * rather than trusted. */
void sd_unmount(void);

/* Initializes raw SDMMC communication with the card *without* attempting a
 * FatFs mount -- unlike sd_mount(), this succeeds even if the card has no
 * recognizable filesystem on it (blank, mid-repartition, wrong filesystem,
 * etc.), which is exactly the state SD passthrough mode is often used to
 * fix. sd_mount() can't be reused for this: ESP-IDF's
 * esp_vfs_fat_sdmmc_mount() frees its internal card object and never hands
 * it back if the filesystem-mount step fails, even though the lower-level
 * SD communication that step depends on already succeeded by that point --
 * so a card with no filesystem previously made sd_get_card() return NULL
 * during passthrough too, defeating the point. Call sd_unmount() first if a
 * FatFs mount is currently active; the two can't be up at once (same
 * physical SDMMC host/slot). */
esp_err_t sd_mount_raw(void);

/* Releases what sd_mount_raw() set up. */
void sd_unmount_raw(void);

/* The current SDMMC card object -- from whichever of sd_mount() or
 * sd_mount_raw() was called most recently, used for direct
 * sdmmc_read_sectors()/write_sectors() access in SD passthrough mode
 * (bypassing FatFs) as well as for capacity/info queries. NULL if neither
 * is currently mounted. */
sdmmc_card_t *sd_get_card(void);

/* Lists *.iso / *.img files at the SD card root into `out` (capacity
 * `max_count`), returns how many were found. Not recursive -- Phase 1 keeps
 * image management to "files at the root of the card". */
size_t sd_list_images(sd_image_t *out, size_t max_count);

uint64_t sd_free_bytes(void);
uint64_t sd_total_bytes(void);
