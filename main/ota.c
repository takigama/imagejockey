#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "ota";

/* GitHub's "latest" release alias -- always redirects to whatever asset is
 * attached to the most recent release under this exact name, so there's no
 * need to hit the GitHub API to resolve a version/tag first for the actual
 * download. Keep this asset filename in sync with what scripts/release.sh
 * uploads and what CMakeLists.txt/idf.py build actually produces
 * (imagejockey.bin). */
#define OTA_URL "https://github.com/takigama/imagejockey/releases/latest/download/imagejockey.bin"
#define OTA_CHECK_URL "https://api.github.com/repos/takigama/imagejockey/releases/latest"

/* Cert validation deliberately disabled for now (no crt_bundle_attach/
 * cert_pem/use_global_ca_store set below, plus CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
 * and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY in sdkconfig.defaults, needed
 * to let esp_https_ota/esp_http_client proceed with no verification method
 * configured) -- found while chasing "Out of buffer" OTA failures on this
 * no-PSRAM board with the USB MSC drive active: the actual fix ended up
 * being the pending-update reboot cycle below (MSC's own memory footprint
 * was the real problem), so this can be re-enabled once confirmed still
 * needed -- the trimmed bundle at certs/github_ota_roots.pem is still
 * there for that. TLS is still encrypted either way, just not
 * authenticated. */

/* Survives esp_restart() (soft reset) but resets to 0 on power loss --
 * exactly the lifetime needed for "do the update on the very next boot,
 * then never again unless asked again." */
#define BOOT_ACTION_MAGIC_UPDATE 0x4F544131u /* "OTA1" */
RTC_NOINIT_ATTR static uint32_t s_boot_action;
static bool s_pending_update_this_boot;

bool ota_update_pending_this_boot(void)
{
    return s_pending_update_this_boot;
}

bool ota_consume_pending_update_flag(void)
{
    s_pending_update_this_boot = (s_boot_action == BOOT_ACTION_MAGIC_UPDATE);
    s_boot_action = 0; /* clear immediately -- a crash mid-update must not loop forever */
    return s_pending_update_this_boot;
}

void ota_schedule_update_reboot(void)
{
    s_boot_action = BOOT_ACTION_MAGIC_UPDATE;
    ESP_LOGW(TAG, "scheduling update reboot (MSC will be skipped next boot)");
    esp_restart();
}

/* Accumulates the response body for the version-check request -- capped,
 * since we only need the "tag_name" field somewhere in GitHub's release
 * JSON, not the whole payload (which includes changelog text, asset lists,
 * etc. and can run to several KB). */
#define CHECK_BODY_CAP 4096
struct check_ctx {
    char *buf;
    size_t len;
};

static esp_err_t check_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        struct check_ctx *ctx = (struct check_ctx *)evt->user_data;
        size_t room = CHECK_BODY_CAP - 1 - ctx->len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if (n > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, n);
            ctx->len += n;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t ota_check_for_update(char *latest_out, size_t latest_out_len, bool *update_available)
{
    *update_available = false;
    latest_out[0] = '\0';

    struct check_ctx ctx = {
        .buf = malloc(CHECK_BODY_CAP),
        .len = 0,
    };
    if (ctx.buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx.buf[0] = '\0';

    esp_http_client_config_t http_cfg = {
        .url = OTA_CHECK_URL,
        .event_handler = check_event_handler,
        .user_data = &ctx,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(ctx.buf);
        return ESP_FAIL;
    }
    /* GitHub's API rejects requests with no User-Agent. */
    esp_http_client_set_header(client, "User-Agent", "ImageJockey-OTA");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update check failed: %s", esp_err_to_name(err));
        free(ctx.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "update check got HTTP %d", status);
        free(ctx.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(ctx.buf);
    free(ctx.buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "update check: couldn't parse response JSON");
        return ESP_FAIL;
    }
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag) || tag->valuestring == NULL) {
        ESP_LOGE(TAG, "update check: no tag_name in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strncpy(latest_out, tag->valuestring, latest_out_len - 1);
    latest_out[latest_out_len - 1] = '\0';
    cJSON_Delete(root);

    *update_available = strcmp(latest_out, FIRMWARE_VERSION) != 0;
    ESP_LOGI(TAG, "update check: running %s, latest %s, update_available=%d", FIRMWARE_VERSION, latest_out,
             (int)*update_available);
    return ESP_OK;
}

esp_err_t ota_update_from_github(void)
{
    esp_http_client_config_t http_cfg = {
        .url = OTA_URL,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "starting OTA from %s (running %s)", OTA_URL, FIRMWARE_VERSION);
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    return err;
}
