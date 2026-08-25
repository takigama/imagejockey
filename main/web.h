#pragma once

#include "esp_err.h"

/* Starts the management HTTP server (image list/select, upload, WiFi
 * config, OTA trigger). Reachable at http://192.168.4.1 (SoftAP) or the
 * station IP once connected -- see wifi.h. */
esp_err_t web_start(void);
