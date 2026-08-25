#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Connects to the saved network (if any credentials are stored) while also
 * always bringing up a SoftAP -- the device is reachable at 192.168.4.1
 * either way, whether or not the station connection succeeds. This is
 * simpler and more robust than switching between AP-only and STA-only
 * modes, and means there's always a known way to reach the device even in
 * the field with no known network. Blocks briefly (a few seconds) if
 * attempting a station connection. */
void wifi_init(void);

bool wifi_is_connected(void);

/* Human-readable current status, e.g. "192.168.1.50" or "AP only: ImageJockey-AB12". */
void wifi_get_status_string(char *out, size_t outlen);

/* Saves new station credentials to NVS and reboots to apply them (simplest
 * way to cleanly restart the WiFi stack in the new mode). */
void wifi_apply_new_credentials(const char *ssid, const char *pass);
