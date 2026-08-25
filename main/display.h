#pragma once

#include <stdint.h>

#define DISPLAY_WIDTH  160
#define DISPLAY_HEIGHT 80

/* 1x (6x8 per character) was too small to read on this physical 0.96" panel;
 * uniform 2x was legible but only fit 6 chars/row, too cramped for real
 * filenames. Tall-but-narrow (2x height, 1x width) keeps the readable row
 * height while getting back to 13 chars/row. */
#define DISPLAY_FONT_SCALE_X 1
#define DISPLAY_FONT_SCALE_Y 2
#define DISPLAY_CHAR_W (6 * DISPLAY_FONT_SCALE_X)
#define DISPLAY_CHAR_H (8 * DISPLAY_FONT_SCALE_Y)

void display_init(void);
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void display_flush(void);

/*
 * Panel is configured with .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE, so the
 * driver handles wire-format byte order itself -- callers just get plain
 * host-order RGB565 here. (If colors come out channel-swapped on real
 * hardware, this is the place to fix it.)
 */
static inline uint16_t display_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
