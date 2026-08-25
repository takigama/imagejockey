#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_timer.h"

/* The board's only button: the BOOT button on GPIO0, active-low. It's also
 * the download-mode strap pin, so it must only be read after boot -- never
 * sample it during/near reset. */
#define BUTTON_GPIO GPIO_NUM_0

#define DEBOUNCE_US    (20 * 1000)
#define LONG_PRESS_US  (600 * 1000)

static bool s_pressed = false;
static int64_t s_press_start_us = 0;
static bool s_long_fired = false;

void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

button_event_t button_poll(void)
{
    bool level_low = gpio_get_level(BUTTON_GPIO) == 0;
    int64_t now = esp_timer_get_time();

    if (level_low && !s_pressed) {
        s_pressed = true;
        s_press_start_us = now;
        s_long_fired = false;
        return BUTTON_EVENT_NONE;
    }

    if (level_low && s_pressed) {
        if (!s_long_fired && (now - s_press_start_us) >= LONG_PRESS_US) {
            s_long_fired = true;
            return BUTTON_EVENT_LONG_PRESS;
        }
        return BUTTON_EVENT_NONE;
    }

    if (!level_low && s_pressed) {
        s_pressed = false;
        int64_t held_us = now - s_press_start_us;
        if (!s_long_fired && held_us >= DEBOUNCE_US) {
            return BUTTON_EVENT_SHORT_PRESS;
        }
        return BUTTON_EVENT_NONE;
    }

    return BUTTON_EVENT_NONE;
}
