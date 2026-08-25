#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "button.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "msc_disk.h"

static const char *TAG = "ui";

/* Fixed-center carousel: 3 rows always on screen, the middle one is always
 * the browse cursor -- short-press scrolls the list past it rather than
 * moving a cursor through a scrolling window. Separate from that is which
 * image is "mounted" (marked with a leading "* "), which only changes on
 * long-press and persists even after you've browsed elsewhere.
 *
 * The list also carries one synthetic trailing entry, "+ NEW IMAGE" --
 * long-pressing it doesn't mount anything, it drops into the size-picker
 * sub-mode instead (see ui_mode_t). */
#define VISIBLE_ROWS 3
#define MIDDLE_ROW   1

static const uint64_t kPresetSizes[] = {
    64ULL << 20,   128ULL << 20,  256ULL << 20, 512ULL << 20,
    1024ULL << 20, 2048ULL << 20, 4000ULL << 20, /* stay under the 4GiB FAT32 ceiling */
};
#define NUM_PRESET_SIZES (sizeof(kPresetSizes) / sizeof(kPresetSizes[0]))
/* One past the last preset, cycled into as a way to back out without
 * creating anything -- single-button UIs need an explicit "never mind". */
#define SIZE_PICKER_CANCEL NUM_PRESET_SIZES

typedef enum {
    UI_MODE_BROWSE,
    UI_MODE_SIZE_PICKER,
} ui_mode_t;

static void format_size(uint64_t bytes, char *out, size_t outlen)
{
    if (bytes >= (1ULL << 30)) {
        unsigned long tenths = (unsigned long)((bytes * 10) / (1ULL << 30));
        snprintf(out, outlen, "%lu.%lug", tenths / 10, tenths % 10);
    } else if (bytes >= (1ULL << 20)) {
        snprintf(out, outlen, "%lum", (unsigned long)(bytes / (1ULL << 20)));
    } else if (bytes >= (1ULL << 10)) {
        snprintf(out, outlen, "%luk", (unsigned long)(bytes / (1ULL << 10)));
    } else {
        snprintf(out, outlen, "%lub", (unsigned long)bytes);
    }
}

static void draw_browse(size_t count, int selected, bool show_selected_flash,
                         uint16_t bg, uint16_t fg, uint16_t accent, int row_h,
                         int text_y_offset, int chars_per_row)
{
    int mounted = media_mounted_index();
    size_t total_rows = count + 1; /* + the synthetic "+ NEW IMAGE" row */

    for (int slot = 0; slot < VISIBLE_ROWS; slot++) {
        int idx = selected + (slot - MIDDLE_ROW);
        int row_y = slot * row_h;

        if (idx < 0 || idx >= (int)total_rows) {
            display_fill_rect(0, row_y, DISPLAY_WIDTH, row_h, bg);
            continue;
        }

        bool is_selected = (slot == MIDDLE_ROW);
        uint16_t row_fg = is_selected ? bg : fg;
        uint16_t row_bg = is_selected ? fg : bg;
        if (is_selected && show_selected_flash) {
            row_fg = bg;
            row_bg = accent;
        }
        display_fill_rect(0, row_y, DISPLAY_WIDTH, row_h, row_bg);

        int text_y = row_y + text_y_offset;

        if (idx == (int)count) { /* synthetic "new image" row */
            display_draw_text(0, text_y, "+ NEW IMAGE", row_fg, row_bg);
            continue;
        }

        char size_str[16];
        format_size(media_size((size_t)idx), size_str, sizeof(size_str));
        int size_chars = (int)strlen(size_str);

        const char *prefix = (idx == mounted) ? "* " : "  ";
        display_draw_text(0, text_y, prefix, row_fg, row_bg);

        int name_max = chars_per_row - 2 /* prefix */ - 1 /* gap */ - size_chars;
        if (name_max < 0) {
            name_max = 0;
        }
        char name[32];
        int n = name_max < (int)sizeof(name) - 1 ? name_max : (int)sizeof(name) - 1;
        strncpy(name, media_display_name((size_t)idx), n);
        name[n] = '\0';
        display_draw_text(2 * DISPLAY_CHAR_W, text_y, name, row_fg, row_bg);

        int size_x = DISPLAY_WIDTH - size_chars * DISPLAY_CHAR_W;
        display_draw_text(size_x, text_y, size_str, row_fg, row_bg);
    }
}

static void draw_size_picker(size_t size_idx, uint16_t bg, uint16_t fg, uint16_t accent)
{
    char label[24];
    if (size_idx == SIZE_PICKER_CANCEL) {
        snprintf(label, sizeof(label), "CANCEL");
    } else {
        char size_str[16];
        format_size(kPresetSizes[size_idx], size_str, sizeof(size_str));
        snprintf(label, sizeof(label), "%s", size_str);
    }

    int title_y = DISPLAY_HEIGHT / 2 - DISPLAY_CHAR_H - 4;
    int value_y = DISPLAY_HEIGHT / 2 + 4;

    display_draw_text(0, title_y, "NEW IMAGE SIZE", accent, bg);
    display_draw_text(0, value_y, label, fg, bg);
}

void ui_run(bool debug_mode)
{
    display_init();
    button_init();
    /* settings_init() already ran in app_main.c, before wifi_init() (which
     * needs NVS up first). */

    const uint16_t bg = display_rgb565(0, 0, 0);
    const uint16_t fg = display_rgb565(255, 255, 255);
    const uint16_t accent = display_rgb565(0, 200, 80);

    esp_err_t sd_err = media_init();

    if (!debug_mode) {
        esp_err_t usb_err = msc_disk_init();
        if (usb_err != ESP_OK) {
            ESP_LOGE(TAG, "msc_disk_init failed: %s", esp_err_to_name(usb_err));
        }
    } else {
        ESP_LOGW(TAG, "debug mode: USB MSC not started, console stays available");
    }

    size_t last_count = media_count();
    int last_mounted = media_mounted_index();
    int selected = last_mounted;
    if (selected < 0) {
        selected = 0;
    }

    ui_mode_t mode = UI_MODE_BROWSE;
    size_t size_idx = 0;

    bool dirty = true;
    bool show_selected_flash = false;
    int64_t flash_until_us = 0;

    const int row_h = DISPLAY_HEIGHT / VISIBLE_ROWS;
    const int text_y_offset = (row_h - DISPLAY_CHAR_H) / 2;
    const int chars_per_row = DISPLAY_WIDTH / DISPLAY_CHAR_W;

    while (1) {
        /* The web UI can create/delete/rename/mount images concurrently --
         * poll for that rather than trusting a count cached from the last
         * time *this* loop changed something. */
        size_t count = media_count();
        int mounted_now = media_mounted_index();
        if (count != last_count || mounted_now != last_mounted) {
            last_count = count;
            last_mounted = mounted_now;
            dirty = true;
        }

        size_t total_rows = count + 1; /* + the synthetic "+ NEW IMAGE" row */
        if ((size_t)selected >= total_rows) {
            selected = (int)(total_rows - 1);
            dirty = true;
        }

        button_event_t ev = button_poll();

        if (mode == UI_MODE_BROWSE) {
            if (ev == BUTTON_EVENT_SHORT_PRESS && total_rows > 0) {
                selected = (int)(((size_t)selected + 1) % total_rows);
                dirty = true;
            } else if (ev == BUTTON_EVENT_LONG_PRESS && total_rows > 0) {
                if ((size_t)selected == count) {
                    mode = UI_MODE_SIZE_PICKER;
                    size_idx = 0;
                } else {
                    media_mount((size_t)selected);
                    show_selected_flash = true;
                    flash_until_us = esp_timer_get_time() + 400000;
                }
                dirty = true;
            }
        } else { /* UI_MODE_SIZE_PICKER */
            if (ev == BUTTON_EVENT_SHORT_PRESS) {
                size_idx = (size_idx + 1) % (NUM_PRESET_SIZES + 1);
                dirty = true;
            } else if (ev == BUTTON_EVENT_LONG_PRESS) {
                if (size_idx != SIZE_PICKER_CANCEL) {
                    /* Preallocating a multi-GB file walks its whole FAT
                     * cluster chain and can take a while -- show something
                     * before the blocking call, not just an apparently
                     * frozen screen. */
                    display_clear(bg);
                    display_draw_text(0, (DISPLAY_HEIGHT - DISPLAY_CHAR_H) / 2, "CREATING...", accent, bg);
                    display_flush();

                    esp_err_t err = media_create_and_mount(kPresetSizes[size_idx]);
                    if (err == ESP_OK) {
                        count = media_count();
                        selected = media_mounted_index();
                        show_selected_flash = true;
                        flash_until_us = esp_timer_get_time() + 400000;
                    } else {
                        ESP_LOGE(TAG, "media_create_and_mount failed: %s", esp_err_to_name(err));
                    }
                }
                mode = UI_MODE_BROWSE;
                dirty = true;
            }
        }

        if (show_selected_flash && esp_timer_get_time() > flash_until_us) {
            show_selected_flash = false;
            dirty = true;
        }

        if (dirty) {
            display_clear(bg);

            if (sd_err != ESP_OK) {
                display_draw_text(0, (DISPLAY_HEIGHT - DISPLAY_CHAR_H) / 2, "SD ERROR", accent, bg);
            } else if (mode == UI_MODE_SIZE_PICKER) {
                draw_size_picker(size_idx, bg, fg, accent);
            } else {
                draw_browse(count, selected, show_selected_flash, bg, fg, accent, row_h, text_y_offset,
                            chars_per_row);
            }

            display_flush();
            dirty = false;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
