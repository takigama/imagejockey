#include "ota.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"

/* GitHub's "latest" release alias -- always redirects to whatever asset is
 * attached to the most recent release under this exact name, so there's no
 * need to hit the GitHub API to resolve a version/tag first. Keep this
 * asset filename in sync with what scripts/release.sh uploads and what
 * CMakeLists.txt/idf.py build actually produces (imagejockey.bin). */
#define OTA_URL "https://github.com/takigama/imagejockey/releases/latest/download/imagejockey.bin"

static const char *TAG = "ota";

esp_err_t ota_update_from_github(void)
{
    esp_http_client_config_t http_cfg = {
        .url = OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        /* esp_http_client's default 512-byte header buffer isn't enough --
         * GitHub's redirect responses carry a multi-KB Content-Security-
         * Policy header, which overflows it ("HTTP_CLIENT: Out of buffer",
         * confirmed via web.c's /log endpoint on a real failed attempt). */
        .buffer_size = 4096,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "starting OTA from %s", OTA_URL);
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    return err;
}
