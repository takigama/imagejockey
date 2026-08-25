#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "logbuf.h"
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
 * SSIDs/passwords, display names, and uploaded filenames commonly contain
 * spaces/symbols that httpd_query_key_value() doesn't decode for you). */
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

/* Appends the JSON-escaped form of `in` to `out` (which must already hold a
 * valid, NUL-terminated string) -- for embedding user-supplied names into
 * the page's inline JS data array safely. */
static void json_append_escaped(char *out, size_t outlen, const char *in)
{
    size_t o = strlen(out);
    for (const char *p = in; *p && o + 2 < outlen; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            continue; /* skip control chars -- simplest safe handling */
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static const char *const kPageHead =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ImageJockey</title>"
    "<style>"
    ":root{--bg:#111;--card:#1a1a1a;--border:#2a2a2a;--fg:#eee;--dim:#888;--accent:#0f8;--danger:#f55;}"
    "*{box-sizing:border-box}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:var(--bg);color:var(--fg);"
    "padding:1em;max-width:520px;margin:auto;line-height:1.4}"
    "h1{font-size:1.3em;margin:0 0 .1em}"
    "h2{font-size:.95em;text-transform:uppercase;letter-spacing:.05em;color:var(--dim);"
    "margin:1.6em 0 .6em;border-bottom:1px solid var(--border);padding-bottom:.3em}"
    ".status{color:var(--dim);font-size:.9em;margin-bottom:.5em}"
    ".status b{color:var(--accent)}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:10px;"
    "padding:.7em .9em;margin-bottom:.5em}"
    ".row{display:flex;align-items:center;gap:.6em}"
    ".grow{flex:1;min-width:0}"
    ".name{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".mounted{color:var(--accent)}"
    ".size{color:var(--dim);font-size:.85em}"
    "button,.btn{background:#222;color:var(--fg);border:1px solid #444;border-radius:6px;"
    "padding:.4em .7em;font-size:.9em;cursor:pointer;white-space:nowrap}"
    "button:active{background:#333}"
    ".btn-primary{border-color:var(--accent);color:var(--accent)}"
    ".btn-danger{border-color:var(--danger);color:var(--danger)}"
    "input,select{background:#222;color:var(--fg);border:1px solid #444;border-radius:6px;"
    "padding:.5em;font-size:.95em;width:100%}"
    "label{display:block;font-size:.85em;color:var(--dim);margin:.6em 0 .2em}"
    ".hint{color:var(--dim);font-size:.8em;margin-top:.4em}"
    ".empty{color:var(--dim);font-style:italic;padding:.5em 0}"
    "#upstatus,#createstatus{font-size:.85em;color:var(--dim);margin-top:.4em}"
    "</style></head><body>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, kPageHead);

    httpd_resp_sendstr_chunk(req, "<h1>ImageJockey</h1><div class='status'>");
    char status[48];
    wifi_get_status_string(status, sizeof(status));
    httpd_resp_sendstr_chunk(req, status);
    httpd_resp_sendstr_chunk(req, "</div>");

    if (media_is_passthrough()) {
        httpd_resp_sendstr_chunk(req,
            "<div class='card'><b class='mounted'>SD passthrough mode active</b>"
            "<p class='hint'>The whole SD card is exposed raw to the USB host, like a normal card "
            "reader -- drag files on directly. Image list/upload/etc. are unavailable until you exit.</p>"
            "<form method='GET' action='/passthrough/off'>"
            "<button class='btn-primary' type='submit'>Exit passthrough mode</button></form></div>"
            "</body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    /* --- Images --- */
    httpd_resp_sendstr_chunk(req, "<h2>Images</h2>");
    size_t count = media_count();
    int mounted = media_mounted_index();

    if (count == 0) {
        httpd_resp_sendstr_chunk(req, "<p class='empty'>No images yet -- create or upload one below.</p>");
    }

    /* JS data array first, so the row markup below can reference it by
     * index without ever interpolating a name into an HTML attribute. */
    static char jsbuf[SD_MAX_IMAGES * (2 * SD_MAX_NAME_LEN + 32) + 64];
    jsbuf[0] = '\0';
    strncat(jsbuf, "<script>const IMAGES=[", sizeof(jsbuf) - strlen(jsbuf) - 1);
    for (size_t i = 0; i < count; i++) {
        strncat(jsbuf, "{\"name\":\"", sizeof(jsbuf) - strlen(jsbuf) - 1);
        json_append_escaped(jsbuf, sizeof(jsbuf), media_name(i));
        strncat(jsbuf, "\",\"display\":\"", sizeof(jsbuf) - strlen(jsbuf) - 1);
        json_append_escaped(jsbuf, sizeof(jsbuf), media_display_name(i));
        strncat(jsbuf, "\"},", sizeof(jsbuf) - strlen(jsbuf) - 1);
    }
    strncat(jsbuf, "];</script>", sizeof(jsbuf) - strlen(jsbuf) - 1);
    httpd_resp_sendstr_chunk(req, jsbuf);

    for (size_t i = 0; i < count; i++) {
        char size_str[16];
        format_size(media_size(i), size_str, sizeof(size_str));

        char row[512];
        if ((int)i == mounted) {
            snprintf(row, sizeof(row),
                     "<div class='card'><div class='row'>"
                     "<div class='grow'><div class='name mounted'>&#9654; %s</div>"
                     "<div class='size'>%s</div></div>"
                     "<button class='btn-primary' disabled>mounted</button>"
                     "<button onclick=\"renameImage(%u)\">rename</button>"
                     "<button class='btn-danger' onclick=\"deleteImage(%u)\">delete</button>"
                     "</div></div>",
                     media_display_name(i), size_str, (unsigned)i, (unsigned)i);
        } else {
            snprintf(row, sizeof(row),
                     "<div class='card'><div class='row'>"
                     "<div class='grow'><div class='name'>%s</div><div class='size'>%s</div></div>"
                     "<a class='btn' href=\"/mount?i=%u\">mount</a>"
                     "<button onclick=\"renameImage(%u)\">rename</button>"
                     "<button class='btn-danger' onclick=\"deleteImage(%u)\">delete</button>"
                     "</div></div>",
                     media_display_name(i), size_str, (unsigned)i, (unsigned)i, (unsigned)i);
        }

        httpd_resp_sendstr_chunk(req, row);
    }

    /* --- Create --- */
    httpd_resp_sendstr_chunk(req,
        "<h2>Create blank image</h2><div class='card'>"
        "<label>Name</label><input id='newname' placeholder='e.g. Scratch disk'>"
        "<label>Size (MB)</label><input id='newsize' type='number' min='1' value='1024'>"
        "<p class='hint'>Creates a writable .img file -- format it from your OS after mounting.</p>"
        "<div style='margin-top:.6em'><button class='btn-primary' onclick='createImage()'>Create</button></div>"
        "<p id='createstatus'></p></div>"

        "<script>"
        "async function renameImage(i){"
        "var img=IMAGES[i];"
        "var name=prompt('Display name for '+img.name+':',img.display);"
        "if(name===null)return;"
        "await fetch('/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'i='+i+'&name='+encodeURIComponent(name)});"
        "location.reload();"
        "}"
        "async function deleteImage(i){"
        "var img=IMAGES[i];"
        "if(!confirm('Delete \"'+img.display+'\" ('+img.name+')? This cannot be undone.'))return;"
        "await fetch('/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'i='+i});"
        "location.reload();"
        "}"
        "async function createImage(){"
        "var mb=document.getElementById('newsize').value;"
        "var name=document.getElementById('newname').value;"
        "var st=document.getElementById('createstatus');"
        "if(!mb||mb<1){alert('enter a size in MB');return;}"
        "st.textContent='creating (this can take a while for large sizes)...';"
        "try{"
        "var res=await fetch('/create',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'size_mb='+encodeURIComponent(mb)+'&name='+encodeURIComponent(name)});"
        "st.textContent=res.ok?'done':'failed';"
        "if(res.ok)location.reload();"
        "}catch(e){st.textContent='failed: '+e;}"
        "}"
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
        "</script>");

    /* --- Upload --- */
    httpd_resp_sendstr_chunk(req,
        "<h2>Upload</h2><div class='card'>"
        "<input type='file' id='f'>"
        "<div style='margin-top:.6em'><button class='btn-primary' onclick='doUpload()'>Upload</button></div>"
        "<p id='upstatus'></p></div>"

        "<h2>SD passthrough</h2><div class='card'>"
        "<p class='hint'>Expose the whole SD card raw to the USB host, like a normal card reader --"
        " useful for large/fast local transfers instead of WiFi. Unmounts whatever's currently mounted"
        " while active.</p>"
        "<form method='GET' action='/passthrough/on'><button type='submit'>Enable SD passthrough</button></form>"
        "</div>"

        "<h2>WiFi</h2><div class='card'>"
        "<form method='POST' action='/wifi'>"
        "<label>SSID</label><input name='ssid'>"
        "<label>Password</label><input name='pass' type='password'>"
        "<div style='margin-top:.6em'><button class='btn-primary' type='submit'>Save &amp; reboot</button></div>"
        "</form></div>"

        "<h2>Firmware</h2><div class='card'>"
        "<form method='GET' action='/ota'><button type='submit'>Check for update (GitHub)</button></form>"
        "</div>"
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

static esp_err_t passthrough_on_get_handler(httpd_req_t *req)
{
    media_enter_passthrough();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t passthrough_off_get_handler(httpd_req_t *req)
{
    media_exit_passthrough();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Reads a small application/x-www-form-urlencoded POST body into `body`. */
static esp_err_t read_form_body(httpd_req_t *req, char *body, size_t bodylen)
{
    size_t len = (size_t)req->content_len < bodylen - 1 ? (size_t)req->content_len : bodylen - 1;
    int received = httpd_req_recv(req, body, len);
    if (received <= 0) {
        return ESP_FAIL;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t rename_post_handler(httpd_req_t *req)
{
    char body[256];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ival[8] = {0};
    char name[SD_MAX_NAME_LEN] = {0};
    httpd_query_key_value(body, "i", ival, sizeof(ival));
    httpd_query_key_value(body, "name", name, sizeof(name));
    url_decode(name);

    int idx = atoi(ival);
    if (idx >= 0 && (size_t)idx < media_count()) {
        media_set_display_name((size_t)idx, name);
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t delete_post_handler(httpd_req_t *req)
{
    char body[64];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ival[8] = {0};
    httpd_query_key_value(body, "i", ival, sizeof(ival));
    int idx = atoi(ival);

    if (idx < 0 || (size_t)idx >= media_count() || media_delete((size_t)idx) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t create_post_handler(httpd_req_t *req)
{
    char body[256];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char mbval[16] = {0};
    char name[SD_MAX_NAME_LEN] = {0};
    httpd_query_key_value(body, "size_mb", mbval, sizeof(mbval));
    httpd_query_key_value(body, "name", name, sizeof(name));
    url_decode(name);

    long mb = strtol(mbval, NULL, 10);
    if (mb < 1) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int idx = media_create((uint64_t)mb * (1ULL << 20), name);
    if (idx < 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[256];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

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

    /* Call first, respond once, with the real outcome -- a response sent
     * *before* this blocking call can't be followed by a second one, so a
     * failure reason had nowhere to go (this bit us: OTA was silently
     * failing and the only trace was a log line behind a console we
     * couldn't reach without the awkward BOOT-hold-at-boot dance). On
     * success ota_update_from_github() calls esp_restart() itself, so the
     * device just drops the connection mid-response -- expected, fine. */
    esp_err_t err = ota_update_from_github();

    char msg[128];
    snprintf(msg, sizeof(msg), "OTA failed: %s", esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", msg);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
    static char buf[10 * 1024];
    logbuf_dump(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
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
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler };
    static const httpd_uri_t mount_uri = { .uri = "/mount", .method = HTTP_GET, .handler = mount_get_handler };
    static const httpd_uri_t rename_uri = { .uri = "/rename", .method = HTTP_POST, .handler = rename_post_handler };
    static const httpd_uri_t delete_uri = { .uri = "/delete", .method = HTTP_POST, .handler = delete_post_handler };
    static const httpd_uri_t create_uri = { .uri = "/create", .method = HTTP_POST, .handler = create_post_handler };
    static const httpd_uri_t passthrough_on_uri = { .uri = "/passthrough/on",
                                                      .method = HTTP_GET,
                                                      .handler = passthrough_on_get_handler };
    static const httpd_uri_t passthrough_off_uri = { .uri = "/passthrough/off",
                                                       .method = HTTP_GET,
                                                       .handler = passthrough_off_get_handler };
    static const httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_post_handler };
    static const httpd_uri_t ota_uri = { .uri = "/ota", .method = HTTP_GET, .handler = ota_get_handler };
    static const httpd_uri_t log_uri = { .uri = "/log", .method = HTTP_GET, .handler = log_get_handler };
    static const httpd_uri_t upload_uri = { .uri = "/upload/*", .method = HTTP_PUT, .handler = upload_put_handler };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &mount_uri);
    httpd_register_uri_handler(server, &rename_uri);
    httpd_register_uri_handler(server, &delete_uri);
    httpd_register_uri_handler(server, &create_uri);
    httpd_register_uri_handler(server, &passthrough_on_uri);
    httpd_register_uri_handler(server, &passthrough_off_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &log_uri);
    httpd_register_uri_handler(server, &upload_uri);

    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
