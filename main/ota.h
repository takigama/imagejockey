#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* This build's own version string (git describe at build time, e.g.
 * "v0.3.0" or "v0.3.0-2-gabc1234-dirty"; "dev" outside a git checkout). */
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

/* Lightweight check against GitHub's "latest release" API (small JSON
 * response, no firmware download) -- cheap enough to run with the USB MSC
 * drive still active. Copies the latest release's tag into latest_out and
 * reports whether it differs from FIRMWARE_VERSION. Returns ESP_OK if the
 * check itself completed (regardless of whether an update is available);
 * an error means the check couldn't be completed at all. */
esp_err_t ota_check_for_update(char *latest_out, size_t latest_out_len, bool *update_available);

/* Downloads and installs the firmware at the project's GitHub Releases
 * "latest" asset URL, verifies it, and reboots on success. Blocking (the
 * download itself takes a while over WiFi). Needs the USB MSC drive to NOT
 * be active -- see app_main.c's pending-update reboot flow, which is what
 * actually calls this; web.c never calls it directly. */
esp_err_t ota_update_from_github(void);

/* True if a "reboot without MSC, run the update, reboot back" cycle is
 * currently pending for this boot -- set right after RTC-flag inspection
 * in app_main.c, before it's cleared. Exposed so web.c can report a
 * clearer status while the cycle is in flight. */
bool ota_update_pending_this_boot(void);

/* Schedules the pending-update reboot cycle: persists a flag through the
 * upcoming soft reset (RTC memory, cleared automatically if power is lost)
 * and restarts. app_main.c checks this flag early next boot, skips
 * installing the USB MSC drive for that one boot, performs the update, and
 * reboots again either way -- back to normal (MSC-enabled) operation. */
void ota_schedule_update_reboot(void);

/* Called once, early in app_main() after logbuf_init(). Reads the RTC flag
 * left by ota_schedule_update_reboot() (if any) and clears it immediately,
 * so a crash mid-update can't loop forever retrying. Returns true if an
 * update is pending for this boot. */
bool ota_consume_pending_update_flag(void);
