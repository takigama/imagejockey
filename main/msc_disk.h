#pragma once

#include "esp_err.h"

/* Installs the TinyUSB driver with our custom MSC-only descriptors. Once
 * this succeeds, the native USB port is claimed for MSC -- the
 * USB-Serial/JTAG console (which shares the same physical D+/D- pins) goes
 * silent, and reflashing requires holding BOOT while plugging in to force
 * the ROM bootloader. */
esp_err_t msc_disk_init(void);

/* Forces the connected host to notice a change of backing file: a "soft"
 * USB disconnect + reconnect (D+ pull-up toggle, not a physical unplug),
 * which makes every OS re-enumerate and re-read from scratch. A no-op if
 * msc_disk_init() was never called (e.g. in debug/console mode). */
void msc_disk_notify_mount_changed(void);
