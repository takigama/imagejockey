#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_MOUNT_POINT  "/sdcard"
#define SD_MAX_IMAGES   32
#define SD_MAX_NAME_LEN 64

typedef struct {
    char name[SD_MAX_NAME_LEN];
    uint64_t size_bytes;
} sd_image_t;

esp_err_t sd_mount(void);

/* Lists *.iso / *.img files at the SD card root into `out` (capacity
 * `max_count`), returns how many were found. Not recursive -- Phase 1 keeps
 * image management to "files at the root of the card". */
size_t sd_list_images(sd_image_t *out, size_t max_count);

uint64_t sd_free_bytes(void);
uint64_t sd_total_bytes(void);
