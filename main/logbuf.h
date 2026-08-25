#pragma once

#include <stddef.h>

/* Captures the last N KB of ESP_LOG output into a RAM ring buffer, still
 * also forwarding to the normal console. This board only has one console
 * (the native USB port), which the app's own MSC driver claims -- so
 * without this, diagnosing anything that goes wrong in a normal (non
 * debug-mode) boot requires the awkward "hold BOOT within ~50ms of a
 * power-on that wasn't itself a BOOT-held power-on" dance. web.c's /log
 * endpoint dumps this buffer instead. Call once, as early as possible in
 * app_main() -- anything logged before this call obviously isn't captured. */
void logbuf_init(void);

/* Copies up to outlen-1 bytes of the captured log (oldest-to-newest) into
 * out, NUL-terminated. Returns the number of bytes copied. */
size_t logbuf_dump(char *out, size_t outlen);
