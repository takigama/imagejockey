#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Owns the SD card's image list plus which one is currently "mounted" (the
 * one whose bytes get served over USB) -- the single source of truth shared
 * between ui.c (browsing/selecting) and msc_disk.c's tud_msc_* callbacks
 * (running in TinyUSB's own task context), guarded by an internal mutex.
 *
 * Mounts by filename, not index -- indices shift if files are added or
 * removed from the card between boots.
 */

/* Mounts the SD card, lists images, and restores the last-mounted image (if
 * any) from NVS -- settings_init() must already have been called. */
esp_err_t media_init(void);

size_t media_count(void);
const char *media_name(size_t index);
uint64_t media_size(size_t index);

/* Index of the currently mounted image, or -1 if none. */
int media_mounted_index(void);

/* Index of the image with this exact filename, or -1 if not found. */
int media_find_by_name(const char *name);

/* Closes any previously open file, opens images[index] (writable if it has
 * a .img extension, read-only for anything else -- see has_img_extension in
 * media.c), persists the choice to NVS, and triggers a USB soft-reconnect
 * (msc_disk_notify_mount_changed) so a connected host re-reads. */
void media_mount(size_t index);

/* Preallocates a new blank .img file of the given size at the SD card root
 * (name auto-generated, e.g. "new_image_001.img"), then mounts it -- same
 * as media_mount() would. The file's content is whatever was already on the
 * card's flash at those clusters (not zeroed) -- fine since the host is
 * expected to format it before use. Returns ESP_ERR_NO_MEM if the image
 * list or the naming scheme's 3-digit counter is full. */
esp_err_t media_create_and_mount(uint64_t size_bytes);

/* Streamed upload from web.c's HTTP handler -- writes straight to the SD
 * card as chunks arrive (no whole-file RAM buffering). Each call is
 * individually mutex-guarded (not the whole session held at once), so
 * boot-mode USB reads can interleave between chunks rather than stalling
 * for the whole upload. media_upload_end(false, ...) deletes the partial
 * file; media_upload_end(true, ...) registers it in the image list. */
esp_err_t media_upload_begin(const char *filename);
esp_err_t media_upload_write(const void *data, size_t len);
void media_upload_end(bool success, const char *filename, uint64_t size_bytes);

/* Used by msc_disk.c's tud_msc_* callbacks -- each internally mutex-guarded. */
bool media_is_mounted(void);
bool media_is_writable(void);
uint32_t media_block_count(void); /* in 512-byte blocks */
int32_t media_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t media_write(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t bufsize);
void media_sync(void); /* flush pending writes -- SCSI SYNCHRONIZE CACHE(10) */
