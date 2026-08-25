#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "media.h"
#include "ota.h"
#include "sdcard.h"
#include "wifi.h"

static const char *TAG = "web";

#define UPLOAD_CHUNK_SIZE 4096

static void format_size(uint64_t bytes, char *out, size_t outlen)
{
    if (bytes >= (1ULL << 30)) {
        unsigned long tenths = (unsigned long)((bytes * 10) / (1ULL << 30));
        snprintf(out, outlen, "%lu.%lug", tenths / 10, tenths % 10);
    } else if (bytes >= (1ULL << 20)) {
        snprintf(out, outlen, "%lum", (unsigned long)(bytes / (1ULL << 20)));
    } else {
        snprintf(out, outlen, "%luk", (unsigned long)(bytes / (1ULL << 10)));
    }
}

/* In-place percent-decode + '+' -> space, for form-urlencoded values (WiFi
 * SSIDs/passwords and uploaded filenames commonly contain spaces/symbols
 * that httpd_query_key_value() doesn't decode for you). */
static void url_decode(char *s)
{
    char *out = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *out++ = ' ';
            s++;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>T-Dongle Media</title>"
        "<style>body{font-family:monospace;background:#111;color:#eee;padding:1em;max-width:480px;margin:auto}"
        "table{width:100%;border-collapse:collapse}td{padding:4px 2px;border-bottom:1px solid #333}"
        "a,button{color:#0f8;background:#222;border:1px solid #0f8;padding:4px 8px;text-decoration:none;"
        "cursor:pointer;font-family:monospace}"
        "input{background:#222;color:#eee;border:1px solid #555;padding:4px;font-family:monospace}"
        "h2,h3{color:#0f8}.mounted{color:#0f8}</style></head><body>");

    httpd_resp_sendstr_chunk(req, "<h2>T-Dongle Media</h2><p>");
    char status[48];
    wifi_get_status_string(status, sizeof(status));
    httpd_resp_sendstr_chunk(req, status);
    httpd_resp_sendstr_chunk(req, "</p><h3>Images</h3><table>");

    size_t count = media_count();
    int mounted = media_mounted_index();
    for (size_t i = 0; i < count; i++) {
        char size_str[16];
        format_size(media_size(i), size_str, sizeof(size_str));
        char row[256];
        if ((int)i == mounted) {
            snprintf(row, sizeof(row),
                     "<tr><td><span class='mounted'>* %s</span></td><td>%s</td>"
                     "<td><span class='mounted'>mounted</span></td></tr>",
                     media_name(i), size_str);
        } else {
            snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td><td><a href=\"/mount?i=%u\">mount</a></td></tr>",
                     media_name(i), size_str, (unsigned)i);
        }
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "</table>");
    if (count == 0) {
        httpd_resp_sendstr_chunk(req, "<p>(no images yet -- upload one below)</p>");
    }

    httpd_resp_sendstr_chunk(req,
        "<h3>Upload</h3>"
        "<input type='file' id='f'> <button onclick='doUpload()'>Upload</button>"
        "<p id='upstatus'></p>"
        "<script>"
        "async function doUpload(){"
        "var f=document.getElementById('f').files[0];"
        "if(!f)return;"
        "var st=document.getElementById('upstatus');"
        "st.textContent='uploading...';"
        "try{"
        "var res=await fetch('/upload/'+encodeURIComponent(f.name),{method:'PUT',body:f});"
        "st.textContent=res.ok?'done':'failed';"
        "if(res.ok)location.reload();"
        "}catch(e){st.textContent='failed: '+e;}"
        "}"
        "</script>"

        "<h3>WiFi</h3>"
        "<form method='POST' action='/wifi'>"
        "SSID <input name='ssid'><br><br>"
        "Password <input name='pass' type='password'><br><br>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form>"

        "<h3>Firmware</h3>"
        "<form method='GET' action='/ota'>"
        "<button type='submit'>Check for update (GitHub)</button>"
        "</form>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t mount_get_handler(httpd_req_t *req)
{
    char query[32];
    char ival[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "i", ival, sizeof(ival)) == ESP_OK) {
        int idx = atoi(ival);
        if (idx >= 0 && (size_t)idx < media_count()) {
            media_mount((size_t)idx);
        }
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    size_t len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int received = httpd_req_recv(req, body, len);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    if (strlen(ssid) == 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Saved. Rebooting to apply -- reconnect to your network's WiFi to reach the device "
                             "again (or the same SoftAP if it fails to connect).");

    wifi_apply_new_credentials(ssid, pass); /* reboots, does not return */
    return ESP_OK;
}

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    if (!wifi_is_connected()) {
        httpd_resp_sendstr(req, "Not connected to a network with internet access -- "
                                 "OTA needs a real WiFi connection, not just the SoftAP.");
        return ESP_OK;
    }

    /* Respond before the blocking download so the browser gets immediate
     * feedback -- the device reboots on success without ever finishing a
     * second response. */
    httpd_resp_sendstr(req, "Checking GitHub for the latest release and downloading if newer... "
                             "the device will reboot automatically if it updates.");

    esp_err_t err = ota_update_from_github();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t upload_put_handler(httpd_req_t *req)
{
    const char *prefix = "/upload/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char filename[SD_MAX_NAME_LEN];
    strncpy(filename, req->uri + prefix_len, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    /* Strip any query string a browser might append. */
    char *q = strchr(filename, '?');
    if (q) {
        *q = '\0';
    }
    url_decode(filename);

    if (strlen(filename) == 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "upload starting: %s (%d bytes)", filename, req->content_len);

    if (media_upload_begin(filename) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[UPLOAD_CHUNK_SIZE];
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            ok = false;
            break;
        }
        if (media_upload_write(buf, (size_t)received) != ESP_OK) {
            ok = false;
            break;
        }
        remaining -= received;
    }

    media_upload_end(ok, filename, (uint64_t)req->content_len);

    if (!ok) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler };
    static const httpd_uri_t mount_uri = { .uri = "/mount", .method = HTTP_GET, .handler = mount_get_handler };
    static const httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_post_handler };
    static const httpd_uri_t ota_uri = { .uri = "/ota", .method = HTTP_GET, .handler = ota_get_handler };
    static const httpd_uri_t upload_uri = { .uri = "/upload/*", .method = HTTP_PUT, .handler = upload_put_handler };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &mount_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &upload_uri);

    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
