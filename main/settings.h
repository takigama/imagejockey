#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Persists which image was last mounted (by filename, not index -- indices
 * shift if files are added/removed from the card) so it can be
 * auto-selected and re-mounted on the next boot. */
void settings_init(void);

bool settings_load_mounted_name(char *out, size_t outlen);
void settings_save_mounted_name(const char *name);

/* WiFi station credentials -- absent until saved via the web UI's WiFi
 * form (or never, in which case the device just stays in SoftAP mode). */
bool settings_load_wifi_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
void settings_save_wifi_creds(const char *ssid, const char *pass);
