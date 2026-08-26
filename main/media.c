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
#define NAMES_FILE  FATFS_DRIVE "/.imgnames.tsv"
#define NAMES_MAX_BYTES 8192

typedef enum {
    MEDIA_MODE_BOOT,
    MEDIA_MODE_PASSTHROUGH,
} media_mode_t;

static const char *TAG = "media";

static sd_image_t s_images[SD_MAX_IMAGES];
static size_t s_count = 0;

static SemaphoreHandle_t s_mutex;
static media_mode_t s_mode = MEDIA_MODE_BOOT;

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

/* Caller must hold s_mutex. Matches filenames in s_images[] against the
 * "filename\tdisplay_name" lines in NAMES_FILE. Missing file (nothing
 * custom saved yet) is not an error. */
static void load_display_names(void)
{
    FIL f;
    if (f_open(&f, NAMES_FILE, FA_READ) != FR_OK) {
        return;
    }

    static char buf[NAMES_MAX_BYTES + 1];
    UINT bytes_read = 0;
    f_read(&f, buf, NAMES_MAX_BYTES, &bytes_read);
    f_close(&f);
    buf[bytes_read] = '\0';

    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }

        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            const char *fname = line;
            char *dname = tab + 1;
            size_t dlen = strlen(dname);
            if (dlen > 0 && dname[dlen - 1] == '\r') {
                dname[dlen - 1] = '\0';
            }

            for (size_t i = 0; i < s_count; i++) {
                if (strcmp(s_images[i].name, fname) == 0) {
                    strncpy(s_images[i].display_name, dname, SD_MAX_NAME_LEN - 1);
                    s_images[i].display_name[SD_MAX_NAME_LEN - 1] = '\0';
                    break;
                }
            }
        }

        line = nl ? nl + 1 : NULL;
    }
}

/* Caller must hold s_mutex. Rewrites the whole file from the current
 * in-memory list -- simple and fine given at most SD_MAX_IMAGES entries. */
static void save_display_names(void)
{
    FIL f;
    if (f_open(&f, NAMES_FILE, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        ESP_LOGE(TAG, "failed to open %s for writing", NAMES_FILE);
        return;
    }

    for (size_t i = 0; i < s_count; i++) {
        if (s_images[i].display_name[0] == '\0') {
            continue;
        }
        char line[2 * SD_MAX_NAME_LEN + 2];
        int n = snprintf(line, sizeof(line), "%s\t%s\n", s_images[i].name, s_images[i].display_name);
        UINT written = 0;
        f_write(&f, line, (UINT)n, &written);
    }

    f_close(&f);
}

/* Caller must hold s_mutex. Returns the new index, or -1 on failure. */
static int create_blank_file_locked(uint64_t size_bytes, const char *display_name)
{
    if (s_count >= SD_MAX_IMAGES) {
        return -1;
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
        return -1;
    }

    char path[sizeof(FATFS_DRIVE) + 1 + SD_MAX_NAME_LEN];
    snprintf(path, sizeof(path), FATFS_DRIVE "/%s", name);

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
        return -1;
    }

    size_t new_index = s_count;
    strncpy(s_images[new_index].name, name, SD_MAX_NAME_LEN - 1);
    s_images[new_index].name[SD_MAX_NAME_LEN - 1] = '\0';
    s_images[new_index].size_bytes = size_bytes;
    s_images[new_index].display_name[0] = '\0';
    if (display_name && display_name[0]) {
        strncpy(s_images[new_index].display_name, display_name, SD_MAX_NAME_LEN - 1);
        s_images[new_index].display_name[SD_MAX_NAME_LEN - 1] = '\0';
    }
    s_count++;

    if (s_images[new_index].display_name[0]) {
        save_display_names();
    }

    return (int)new_index;
}

esp_err_t media_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    esp_err_t err = sd_mount();
    if (err != ESP_OK) {
        return err;
    }
    s_count = sd_list_images(s_images, SD_MAX_IMAGES);
    load_display_names();
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

const char *media_display_name(size_t index)
{
    return s_images[index].display_name[0] ? s_images[index].display_name : s_images[index].name;
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
    s_mode = MEDIA_MODE_BOOT;
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

int media_create(uint64_t size_bytes, const char *display_name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = create_blank_file_locked(size_bytes, display_name);
    xSemaphoreGive(s_mutex);

    if (idx >= 0) {
        ESP_LOGI(TAG, "created: %s", s_images[idx].name);
    }
    return idx;
}

esp_err_t media_create_and_mount(uint64_t size_bytes)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_mode = MEDIA_MODE_BOOT;
    int idx = create_blank_file_locked(size_bytes, NULL);
    bool ok = false;
    if (idx >= 0) {
        ok = open_index((size_t)idx);
    }
    xSemaphoreGive(s_mutex);

    if (idx < 0 || !ok) {
        return ESP_FAIL;
    }

    s_mounted = idx;
    settings_save_mounted_name(s_images[idx].name);
    ESP_LOGI(TAG, "created and mounted: %s (%llu bytes)", s_images[idx].name, (unsigned long long)size_bytes);
    msc_disk_notify_mount_changed();
    return ESP_OK;
}

esp_err_t media_delete(size_t index)
{
    if (index >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[sizeof(FATFS_DRIVE) + 1 + SD_MAX_NAME_LEN];
    snprintf(path, sizeof(path), FATFS_DRIVE "/%s", s_images[index].name);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if ((int)index == s_mounted) {
        close_current();
        s_mounted = -1;
    }

    FRESULT res = f_unlink(path);
    if (res == FR_OK) {
        for (size_t i = index; i + 1 < s_count; i++) {
            s_images[i] = s_images[i + 1];
        }
        s_count--;
        if (s_mounted > (int)index) {
            s_mounted--;
        }
        save_display_names();
    }

    xSemaphoreGive(s_mutex);

    if (res != FR_OK) {
        ESP_LOGE(TAG, "delete %s failed: %d", path, res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "deleted: %s", path);
    if (index == (size_t)s_mounted + 1 || s_mounted == -1) {
        /* mounted image was removed (or nothing was mounted) -- let the
         * host notice there's no media right now rather than silently keep
         * serving whatever was in the (now-closed) file handle. */
        msc_disk_notify_mount_changed();
    }
    return ESP_OK;
}

void media_set_display_name(size_t index, const char *display_name)
{
    if (index >= s_count) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (display_name && display_name[0]) {
        strncpy(s_images[index].display_name, display_name, SD_MAX_NAME_LEN - 1);
        s_images[index].display_name[SD_MAX_NAME_LEN - 1] = '\0';
    } else {
        s_images[index].display_name[0] = '\0';
    }
    save_display_names();
    xSemaphoreGive(s_mutex);
}

esp_err_t media_enter_passthrough(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    close_current();
    sd_unmount(); /* release FatFs's hold on the hardware before raw-initing it */
    esp_err_t err = sd_mount_raw();
    if (err == ESP_OK) {
        s_mode = MEDIA_MODE_PASSTHROUGH;
    }
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "entering SD passthrough mode -- whole card exposed raw to host");
        msc_disk_notify_mount_changed();
    } else {
        ESP_LOGE(TAG, "failed to enter SD passthrough mode: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t media_exit_passthrough(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    sd_unmount_raw();
    esp_err_t err = sd_mount();
    if (err == ESP_OK) {
        s_count = sd_list_images(s_images, SD_MAX_IMAGES);
        load_display_names();
    } else {
        s_count = 0;
    }
    s_mounted = -1; /* host changed who-knows-what; stay explicit rather than guess */
    s_mode = MEDIA_MODE_BOOT;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "exited SD passthrough mode, re-mounted (%s), found %d image(s)",
             err == ESP_OK ? "ok" : esp_err_to_name(err), (int)s_count);
    msc_disk_notify_mount_changed();
    return err;
}

bool media_is_passthrough(void)
{
    return s_mode == MEDIA_MODE_PASSTHROUGH;
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
        s_images[s_count].display_name[0] = '\0';
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
    bool ready = (s_mode == MEDIA_MODE_PASSTHROUGH) ? (sd_get_card() != NULL) : s_file_open;
    xSemaphoreGive(s_mutex);
    return ready;
}

bool media_is_writable(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool writable = (s_mode == MEDIA_MODE_PASSTHROUGH) ? true : s_writable;
    xSemaphoreGive(s_mutex);
    return writable;
}

uint32_t media_block_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t blocks;
    if (s_mode == MEDIA_MODE_PASSTHROUGH) {
        sdmmc_card_t *card = sd_get_card();
        blocks = card ? (uint32_t)card->csd.capacity : 0;
    } else {
        blocks = (uint32_t)(s_mounted_size / BLOCK_SIZE);
    }
    xSemaphoreGive(s_mutex);
    return blocks;
}

/* Both read/write below assume exactly one 512-byte block per call (offset
 * always 0) -- true as long as CFG_TUD_MSC_BUFSIZE stays 512 (see
 * components/esp_tinyusb_core/include/tusb_config.h), which is what makes
 * passthrough's 1:1 lba-to-sdmmc-sector mapping valid without needing to
 * handle partial-block offsets or multi-sector transfers per callback. */

int32_t media_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int32_t result;
    if (s_mode == MEDIA_MODE_PASSTHROUGH) {
        sdmmc_card_t *card = sd_get_card();
        esp_err_t err = card ? sdmmc_read_sectors(card, buffer, lba, bufsize / BLOCK_SIZE) : ESP_FAIL;
        result = (err == ESP_OK) ? (int32_t)bufsize : -1;
    } else if (s_file_open) {
        FSIZE_t pos = (FSIZE_t)lba * BLOCK_SIZE + offset;
        FRESULT res = f_lseek(&s_file, pos);
        UINT bytes_done = 0;
        if (res == FR_OK) {
            res = f_read(&s_file, buffer, bufsize, &bytes_done);
        }
        result = (res == FR_OK) ? (int32_t)bytes_done : -1;
    } else {
        result = -1;
    }

    xSemaphoreGive(s_mutex);

    if (result < 0) {
        ESP_LOGE(TAG, "read failed at lba=%lu", (unsigned long)lba);
    }
    return result;
}

int32_t media_write(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t bufsize)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int32_t result;
    if (s_mode == MEDIA_MODE_PASSTHROUGH) {
        sdmmc_card_t *card = sd_get_card();
        esp_err_t err = card ? sdmmc_write_sectors(card, buffer, lba, bufsize / BLOCK_SIZE) : ESP_FAIL;
        result = (err == ESP_OK) ? (int32_t)bufsize : -1;
    } else if (s_file_open && s_writable) {
        FSIZE_t pos = (FSIZE_t)lba * BLOCK_SIZE + offset;
        FRESULT res = f_lseek(&s_file, pos);
        UINT bytes_done = 0;
        if (res == FR_OK) {
            res = f_write(&s_file, buffer, bufsize, &bytes_done);
        }
        result = (res == FR_OK) ? (int32_t)bytes_done : -1;
    } else {
        result = -1;
    }

    xSemaphoreGive(s_mutex);

    if (result < 0) {
        ESP_LOGE(TAG, "write failed at lba=%lu", (unsigned long)lba);
    }
    return result;
}

void media_sync(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == MEDIA_MODE_BOOT && s_file_open && s_writable) {
        f_sync(&s_file);
    }
    xSemaphoreGive(s_mutex);
}
