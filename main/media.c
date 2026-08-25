#include "media.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "msc_disk.h"
#include "sdcard.h"
#include "settings.h"

#define BLOCK_SIZE  512
#define FATFS_DRIVE "0:" /* the only FATFS volume this project mounts */

static const char *TAG = "media";

static sd_image_t s_images[SD_MAX_IMAGES];
static size_t s_count = 0;

static SemaphoreHandle_t s_mutex;
static FIL s_file;
static bool s_file_open = false;
static bool s_writable = false;
static uint64_t s_mounted_size = 0;
static int s_mounted = -1;

static FIL s_upload_file;
static bool s_upload_open = false;

static bool has_img_extension(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }
    return strcasecmp(name + len - 4, ".img") == 0;
}

/* Caller must hold s_mutex. */
static void close_current(void)
{
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = false;
    }
    s_writable = false;
    s_mounted_size = 0;
}

/* Caller must hold s_mutex. */
static bool open_index(size_t index)
{
    close_current();

    char path[sizeof(FATFS_DRIVE) + 1 + SD_MAX_NAME_LEN];
    snprintf(path, sizeof(path), FATFS_DRIVE "/%s", s_images[index].name);

    bool writable = has_img_extension(s_images[index].name);
    BYTE mode = FA_READ | (writable ? FA_WRITE : 0);

    FRESULT res = f_open(&s_file, path, mode);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_open(%s) failed: %d", path, res);
        return false;
    }

    s_file_open = true;
    s_writable = writable;
    s_mounted_size = (uint64_t)f_size(&s_file);
    return true;
}

esp_err_t media_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    esp_err_t err = sd_mount();
    if (err != ESP_OK) {
        return err;
    }
    s_count = sd_list_images(s_images, SD_MAX_IMAGES);
    ESP_LOGI(TAG, "found %d image(s) on SD card", (int)s_count);

    char last_name[SD_MAX_NAME_LEN];
    if (settings_load_mounted_name(last_name, sizeof(last_name))) {
        for (size_t i = 0; i < s_count; i++) {
            if (strcmp(s_images[i].name, last_name) == 0) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                bool ok = open_index(i);
                xSemaphoreGive(s_mutex);
                if (ok) {
                    s_mounted = (int)i;
                    ESP_LOGI(TAG, "restored mounted image: %s", s_images[i].name);
                }
                break;
            }
        }
    }

    return ESP_OK;
}

size_t media_count(void)
{
    return s_count;
}

const char *media_name(size_t index)
{
    return s_images[index].name;
}

uint64_t media_size(size_t index)
{
    return s_images[index].size_bytes;
}

int media_mounted_index(void)
{
    return s_mounted;
}

int media_find_by_name(const char *name)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_images[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void media_mount(size_t index)
{
    if (index >= s_count) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = open_index(index);
    xSemaphoreGive(s_mutex);

    if (!ok) {
        s_mounted = -1;
        return;
    }

    s_mounted = (int)index;
    settings_save_mounted_name(s_images[index].name);
    ESP_LOGI(TAG, "mounted: %s (%s)", s_images[index].name, s_writable ? "writable" : "read-only");
    msc_disk_notify_mount_changed();
}

esp_err_t media_create_and_mount(uint64_t size_bytes)
{
    if (s_count >= SD_MAX_IMAGES) {
        return ESP_ERR_NO_MEM;
    }

    char name[SD_MAX_NAME_LEN];
    int n;
    for (n = 1; n <= 999; n++) {
        snprintf(name, sizeof(name), "new_image_%03d.img", n);
        bool exists = false;
        for (size_t i = 0; i < s_count; i++) {
            if (strcmp(s_images[i].name, name) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            break;
        }
    }
    if (n > 999) {
        return ESP_ERR_NO_MEM;
    }

    char path[sizeof(FATFS_DRIVE) + 1 + SD_MAX_NAME_LEN];
    snprintf(path, sizeof(path), FATFS_DRIVE "/%s", name);

    /* Everything below touches the FatFs volume, which isn't reentrant --
     * held for the whole operation (not just via media_mount(), which would
     * self-deadlock on this same non-recursive mutex) because the host can
     * be polling the currently-mounted file from TinyUSB's task at any
     * moment, including mid-creation. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    FIL f;
    FRESULT res = f_open(&f, path, FA_CREATE_NEW | FA_WRITE);
    if (res == FR_OK) {
        /* Seek to the last byte and write it -- extends the file to the
         * full size via FAT cluster-chain allocation without writing real
         * data to every intervening cluster (much faster than a real
         * zero-fill for a multi-GB file). */
        res = f_lseek(&f, (FSIZE_t)(size_bytes - 1));
        if (res == FR_OK) {
            UINT written = 0;
            uint8_t zero = 0;
            res = f_write(&f, &zero, 1, &written);
        }
        f_close(&f);
    }

    if (res != FR_OK) {
        ESP_LOGE(TAG, "create/preallocate %s failed: %d", path, res);
        f_unlink(path);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    strncpy(s_images[s_count].name, name, SD_MAX_NAME_LEN - 1);
    s_images[s_count].name[SD_MAX_NAME_LEN - 1] = '\0';
    s_images[s_count].size_bytes = size_bytes;
    size_t new_index = s_count;
    s_count++;

    bool ok = open_index(new_index);
    xSemaphoreGive(s_mutex);

    if (!ok) {
        s_mounted = -1;
        return ESP_FAIL;
    }

    s_mounted = (int)new_index;
    settings_save_mounted_name(name);
    ESP_LOGI(TAG, "created and mounted: %s (%llu bytes)", name, (unsigned long long)size_bytes);
    msc_disk_notify_mount_changed();
    return ESP_OK;
}

esp_err_t media_upload_begin(const char *filename)
{
    char path[sizeof(FATFS_DRIVE) + 1 + SD_MAX_NAME_LEN];
    snprintf(path, sizeof(path), FATFS_DRIVE "/%s", filename);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FRESULT res = f_open(&s_upload_file, path, FA_CREATE_ALWAYS | FA_WRITE);
    xSemaphoreGive(s_mutex);

    if (res != FR_OK) {
        ESP_LOGE(TAG, "upload_begin(%s) failed: %d", path, res);
        return ESP_FAIL;
    }
    s_upload_open = true;
    return ESP_OK;
}

esp_err_t media_upload_write(const void *data, size_t len)
{
    if (!s_upload_open) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    UINT written = 0;
    FRESULT res = f_write(&s_upload_file, data, len, &written);
    xSemaphoreGive(s_mutex);

    if (res != FR_OK || written != len) {
        ESP_LOGE(TAG, "upload_write failed: %d", res);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void media_upload_end(bool success, const char *filename, uint64_t size_bytes)
{
    if (!s_upload_open) {
        return;
    }

    char path[sizeof(FATFS_DRIVE) + 1 + SD_MAX_NAME_LEN];
    snprintf(path, sizeof(path), FATFS_DRIVE "/%s", filename);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    f_close(&s_upload_file);
    s_upload_open = false;

    if (success && s_count < SD_MAX_IMAGES) {
        strncpy(s_images[s_count].name, filename, SD_MAX_NAME_LEN - 1);
        s_images[s_count].name[SD_MAX_NAME_LEN - 1] = '\0';
        s_images[s_count].size_bytes = size_bytes;
        s_count++;
        ESP_LOGI(TAG, "upload complete: %s (%llu bytes)", filename, (unsigned long long)size_bytes);
    } else if (!success) {
        ESP_LOGW(TAG, "upload aborted, removing partial file: %s", filename);
        f_unlink(path);
    }
    xSemaphoreGive(s_mutex);
}

bool media_is_mounted(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool mounted = s_file_open;
    xSemaphoreGive(s_mutex);
    return mounted;
}

bool media_is_writable(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool writable = s_writable;
    xSemaphoreGive(s_mutex);
    return writable;
}

uint32_t media_block_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t blocks = (uint32_t)(s_mounted_size / BLOCK_SIZE);
    xSemaphoreGive(s_mutex);
    return blocks;
}

int32_t media_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_file_open) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    FSIZE_t pos = (FSIZE_t)lba * BLOCK_SIZE + offset;
    FRESULT res = f_lseek(&s_file, pos);
    UINT bytes_done = 0;
    if (res == FR_OK) {
        res = f_read(&s_file, buffer, bufsize, &bytes_done);
    }

    xSemaphoreGive(s_mutex);

    if (res != FR_OK) {
        ESP_LOGE(TAG, "read failed at lba=%lu: %d", (unsigned long)lba, res);
        return -1;
    }
    return (int32_t)bytes_done;
}

int32_t media_write(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t bufsize)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_file_open || !s_writable) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    FSIZE_t pos = (FSIZE_t)lba * BLOCK_SIZE + offset;
    FRESULT res = f_lseek(&s_file, pos);
    UINT bytes_done = 0;
    if (res == FR_OK) {
        res = f_write(&s_file, buffer, bufsize, &bytes_done);
    }

    xSemaphoreGive(s_mutex);

    if (res != FR_OK) {
        ESP_LOGE(TAG, "write failed at lba=%lu: %d", (unsigned long)lba, res);
        return -1;
    }
    return (int32_t)bytes_done;
}

void media_sync(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_file_open && s_writable) {
        f_sync(&s_file);
    }
    xSemaphoreGive(s_mutex);
}
