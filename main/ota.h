#pragma once

#include "esp_err.h"

/* Downloads and installs the firmware at the project's GitHub Releases
 * "latest" asset URL, verifies it, and reboots on success. Blocking (the
 * download itself takes a while over WiFi) -- call only when WiFi is
 * connected, and only in response to an explicit user action (the web UI's
 * "Check for update" button), never automatically. */
esp_err_t ota_update_from_github(void);
