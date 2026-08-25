#pragma once

#include <stdbool.h>

/* Browse/select UI: mounts the SD card, lists images, and lets the button
 * cycle/confirm a selection on the TFT. Long-press mounts the selection as
 * the active USB MSC backing store (unless debug_mode is set, in which case
 * USB MSC is never started -- see app_main.c's boot-hold check -- so the
 * USB-Serial/JTAG console stays available instead). Never returns. */
void ui_run(bool debug_mode);
