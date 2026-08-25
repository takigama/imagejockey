#include "sdcard.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

/* Pins per the official LilyGO T-Dongle-S3 examples (sd_card.ino) --
 * SDMMC 4-bit, a separate peripheral from the display's SPI bus. */
#define PIN_SD_CLK 12
#define PIN_SD_CMD 16
#define PIN_SD_D0  14
#define PIN_SD_D1  17
#define PIN_SD_D2  21
#define PIN_SD_D3  18

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_D0;
    slot_config.d1 = PIN_SD_D1;
    slot_config.d2 = PIN_SD_D2;
    slot_config.d3 = PIN_SD_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (card mount timeouts on this board have been "
                       "reported to be arduino-esp32-core-version-sensitive when using "
                       "SD_MMC; if this happens, first suspect wiring/card before pins)",
                 esp_err_to_name(err));
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

sdmmc_card_t *sd_get_card(void)
{
    return s_card;
}

static bool has_image_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }
    const char *ext = name + len - 4;
    return strcasecmp(ext, ".iso") == 0 || strcasecmp(ext, ".img") == 0;
}

size_t sd_list_images(sd_image_t *out, size_t max_count)
{
    size_t count = 0;
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "opendir(%s) failed", SD_MOUNT_POINT);
        return 0;
    }

    struct dirent *entry;
    while (count < max_count && (entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG || !has_image_ext(entry->d_name)) {
            continue;
        }

        /* Sized off dirent's own d_name capacity, not our SD_MAX_NAME_LEN --
         * GCC's format-truncation check reasons about the field's declared
         * size, and a too-small buffer here would just be silently wrong
         * rather than caught at compile time. */
        char path[sizeof(SD_MOUNT_POINT) + 1 + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        strncpy(out[count].name, entry->d_name, SD_MAX_NAME_LEN - 1);
        out[count].name[SD_MAX_NAME_LEN - 1] = '\0';
        out[count].display_name[0] = '\0';
        out[count].size_bytes = (uint64_t)st.st_size;
        count++;
    }
    closedir(dir);
    return count;
}

uint64_t sd_free_bytes(void)
{
    FATFS *fs;
    DWORD free_clusters;
    /* ESP-IDF's default ffconf.h fixes the FAT sector size at 512 bytes
     * (FF_MIN_SS == FF_MAX_SS), so this multiplication is safe without
     * reading fs->ssize. */
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) {
        return 0;
    }
    return (uint64_t)free_clusters * fs->csize * 512;
}

uint64_t sd_total_bytes(void)
{
    if (!s_card) {
        return 0;
    }
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}
