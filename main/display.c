#include "display.h"
#include "font5x7.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"

/* Pins per the official LilyGO T-Dongle-S3 examples (lcd.ino) -- no pin
 * conflict with the SD card, which is on the separate SDMMC peripheral. */
#define PIN_LCD_MOSI 3
#define PIN_LCD_SCLK 5
#define PIN_LCD_CS   4
#define PIN_LCD_DC   2
#define PIN_LCD_RST  1
#define PIN_LCD_BL   38

#define LCD_SPI_HOST     SPI2_HOST
#define LCD_PIXEL_CLK_HZ (20 * 1000 * 1000)

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;

void display_init(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    /* Backlight is active-LOW on this board (confirmed against LilyGO's own
     * lcd.ino, which defines LCD_BK_LIGHT_ON as 0) -- HIGH keeps it off
     * until the first frame is ready. */
    gpio_set_level(PIN_LCD_BL, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * (int)sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    /* LilyGO's own examples/community notes report a 26/1 gap for this exact
     * 80x160 panel (the ST7735 controller's native addressable area is
     * larger than the visible glass). Confirmed/adjusted on first bring-up
     * if the image comes out offset or cropped. */
    /* Landscape orientation (matches LilyGO's lcd.ino primary/tested branch,
     * not the portrait one they left commented out -- that's what needed
     * the mirror-axis correction below when we were using it). */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 1, 26));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_fb = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fb) {
        ESP_LOGE(TAG, "failed to allocate framebuffer");
        abort();
    }

    display_clear(display_rgb565(0, 0, 0));
    display_flush();
    gpio_set_level(PIN_LCD_BL, 0); /* LOW = on, per the note above */
}

void display_clear(uint16_t color)
{
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        s_fb[i] = color;
    }
}

static inline void put_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }
    s_fb[y * DISPLAY_WIDTH + x] = color;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put_pixel(x + i, y + j, color);
        }
    }
}

static inline void put_block(int x, int y, int w, int h, uint16_t color)
{
    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            put_pixel(x + sx, y + sy, color);
        }
    }
}

static void draw_char(int x, int y, unsigned char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *glyph = &font5x7[c * 5];
    const int sx = DISPLAY_FONT_SCALE_X;
    const int sy = DISPLAY_FONT_SCALE_Y;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            put_block(x + col * sx, y + row * sy, sx, sy, (bits & (1 << row)) ? fg : bg);
        }
    }
    for (int row = 0; row < 7; row++) {
        put_block(x + 5 * sx, y + row * sy, sx, sy, bg); /* inter-character spacing */
    }
}

void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        draw_char(cx, y, (unsigned char)*p, fg, bg);
        cx += DISPLAY_CHAR_W;
    }
}

void display_flush(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_fb));
}
