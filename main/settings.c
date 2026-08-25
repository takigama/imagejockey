#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "media"
#define NVS_KEY_MOUNTED "mounted_name"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"

static const char *TAG = "settings";

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

bool settings_load_mounted_name(char *out, size_t outlen)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false; /* namespace doesn't exist yet -- nothing saved */
    }

    size_t required = outlen;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_MOUNTED, out, &required);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }
    return true;
}

void settings_save_mounted_name(const char *name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_MOUNTED, name));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}

bool settings_load_wifi_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t ssid_required = ssid_len;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_WIFI_SSID, ssid, &ssid_required);
    if (err == ESP_OK) {
        size_t pass_required = pass_len;
        err = nvs_get_str(handle, NVS_KEY_WIFI_PASS, pass, &pass_required);
    }
    nvs_close(handle);

    return err == ESP_OK;
}

void settings_save_wifi_creds(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_WIFI_PASS, pass));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}
