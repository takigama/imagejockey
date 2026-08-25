#include "logbuf.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOGBUF_SIZE (12 * 1024)

static char s_buf[LOGBUF_SIZE];
static size_t s_head = 0; /* next write position, wraps */
static size_t s_len = 0;  /* how much of the buffer is valid (<= LOGBUF_SIZE) */
static bool s_wrapped = false;
static SemaphoreHandle_t s_mutex;
static vprintf_like_t s_prev_vprintf;

static void append(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head] = data[i];
        s_head = (s_head + 1) % LOGBUF_SIZE;
        if (s_len < LOGBUF_SIZE) {
            s_len++;
        } else {
            s_wrapped = true;
        }
    }
}

static int logbuf_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        /* Non-blocking take -- logging happens from all sorts of contexts,
         * never want this capture to be why something stalls. Missing an
         * occasional line under contention is an acceptable trade-off. */
        if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
            append(line, len);
            xSemaphoreGive(s_mutex);
        }
    }

    return s_prev_vprintf ? s_prev_vprintf(fmt, args) : n;
}

void logbuf_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(logbuf_vprintf);
}

size_t logbuf_dump(char *out, size_t outlen)
{
    if (outlen == 0) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t start = s_wrapped ? s_head : 0;
    size_t avail = s_len;
    size_t to_copy = avail < outlen - 1 ? avail : outlen - 1;
    /* If the caller's buffer is smaller than what we have, keep the
     * newest content (skip the oldest) -- that's usually what you want
     * when tailing a log. */
    size_t skip = avail - to_copy;
    size_t read_pos = (start + skip) % LOGBUF_SIZE;

    for (size_t i = 0; i < to_copy; i++) {
        out[i] = s_buf[read_pos];
        read_pos = (read_pos + 1) % LOGBUF_SIZE;
    }
    xSemaphoreGive(s_mutex);

    out[to_copy] = '\0';
    return to_copy;
}
