#include <stdio.h>
#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "ui.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "bringup";

/* Once ui_run() installs the TinyUSB MSC driver, the native USB port's
 * pins switch over to it and the USB-Serial/JTAG console goes silent --
 * that's the only console this board has (no separate UART bridge). Holding
 * BOOT (GPIO0) for the first ~50ms after boot skips MSC entirely so the
 * console stays available, e.g. for a debugging session. */
static bool boot_button_held(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(50));
    return gpio_get_level(GPIO_NUM_0) == 0;
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "chip: %s, %d core(s), rev v%d.%d",
             CONFIG_IDF_TARGET, chip_info.cores,
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "flash: %" PRIu32 " MB", flash_size / (1024 * 1024));

#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "psram: %" PRIu32 " MB", (uint32_t)esp_psram_get_size() / (1024 * 1024));
    } else {
        ESP_LOGI(TAG, "psram: CONFIG_SPIRAM enabled but none detected");
    }
#else
    ESP_LOGI(TAG, "psram: CONFIG_SPIRAM not enabled in this build (enable via menuconfig to probe for it)");
#endif

    /* Reaching this point means the app booted and ran far enough to log --
     * cancel the auto-rollback watchdog (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
     * so a fresh OTA image doesn't get silently reverted to the previous one. */
    esp_ota_mark_app_valid_cancel_rollback();

    bool debug_mode = boot_button_held();
    if (debug_mode) {
        ESP_LOGW(TAG, "BOOT held at startup -- staying in debug/console mode, USB MSC not started");
    }

    /* Must run before wifi_init() -- esp_wifi_init() needs NVS initialized
     * (calibration data, stored config) and this is what calls
     * nvs_flash_init(). Previously this only happened inside ui_run(),
     * which runs after wifi_init() -- esp_wifi_init() was likely aborting
     * (wrapped in ESP_ERROR_CHECK) before anything else ever got a chance
     * to start. */
    settings_init();

    wifi_init();
    esp_err_t web_err = web_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web_start failed: %s", esp_err_to_name(web_err));
    }

    ui_run(debug_mode); /* does not return */
}
